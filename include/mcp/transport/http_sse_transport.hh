#pragma once
#include "mcp/transport/transport.hh"
#include "mcp/server/mcp_shard.hh"
#include "mcp/server/mcp_server.hh"
#include <seastar/core/future.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/http/httpd.hh>
#include <seastar/http/handlers.hh>
#include <seastar/http/function_handlers.hh>
#include <seastar/util/log.hh>
#include <memory>
#include <string>

namespace mcp::transport {

inline seastar::logger http_sse_log("http_sse_transport");

// HttpSseTransport：多核 HTTP/SSE 传输
//
// 使用 http_server_control（内部是 sharded<http_server>），
// 每个核心独立接受 HTTP 连接，处理时访问本核的 McpShard。
//
// SSE session 路由规则：
//   - GET /sse  在哪个核处理，session 就归属哪个核
//   - POST /message 解析 sessionId 中的 shard ID
//       同核：直接 push
//       跨核：invoke_on(target) + run_in_background
class HttpSseTransport : public ITransport {
    uint16_t _port;
    seastar::httpd::http_server_control _server;

public:
    explicit HttpSseTransport(uint16_t port = 8080) : _port(port) {}

    seastar::future<> start(mcp::server::McpServer& server) override {
        co_await _server.start("mcp_http_sse");
        // set_routes 的 lambda 在每个 shard 上执行一次，各 shard 独立设置路由
        co_await _server.set_routes([&shards = server.shards()](seastar::httpd::routes& r) {
            _setup_routes(r, shards);
        });
        http_sse_log.info("HttpSseTransport listening on port {}", _port);
        co_await _server.listen(seastar::socket_address{seastar::ipv4_addr{_port}});
    }

    seastar::future<> stop() override {
        return _server.stop();
    }

private:
    static void _setup_routes(seastar::httpd::routes& r,
                               seastar::sharded<mcp::server::McpShard>& shards) {
        r.add(seastar::httpd::operation_type::GET, seastar::httpd::url("/sse"),
            new seastar::httpd::function_handler(
                [&shards](std::unique_ptr<seastar::http::request> req,
                          std::unique_ptr<seastar::http::reply> rep)
                    -> seastar::future<std::unique_ptr<seastar::http::reply>> {
                    return _handle_sse(shards, std::move(req), std::move(rep));
                }, "txt"));

        r.add(seastar::httpd::operation_type::POST, seastar::httpd::url("/message"),
            new seastar::httpd::function_handler(
                [&shards](std::unique_ptr<seastar::http::request> req,
                          std::unique_ptr<seastar::http::reply> rep)
                    -> seastar::future<std::unique_ptr<seastar::http::reply>> {
                    return _handle_message(shards, std::move(req), std::move(rep));
                }, "json"));
    }

    // ── SSE 连接处理 ─────────────────────────────────────────────────────────

    static seastar::future<std::unique_ptr<seastar::http::reply>>
    _handle_sse(seastar::sharded<mcp::server::McpShard>& shards,
                std::unique_ptr<seastar::http::request>,
                std::unique_ptr<seastar::http::reply> rep) {
        // 在本核创建 session
        std::string session_id = shards.local().create_session();
        auto session = shards.local().get_session(session_id);

        rep->set_content_type("text/event-stream");
        rep->add_header("Cache-Control", "no-cache");
        rep->add_header("Connection", "keep-alive");

        rep->write_body("text/event-stream",
            [&shards, session_id, session](seastar::output_stream<char> out) mutable
                -> seastar::future<> {
                try {
                    co_await out.write("event: endpoint\ndata: /message?sessionId="
                                       + session_id + "\n\n");
                    co_await out.flush();
                    while (session->active) {
                        auto msg = co_await session->messages.pop_eventually();
                        if (msg.empty() || !session->active) break;
                        co_await out.write("data: " + msg + "\n\n");
                        co_await out.flush();
                    }
                } catch (...) {
                    session->active = false;
                }
                // 断连后清理本核 session 及其订阅
                shards.local().cleanup_subscriptions(session_id);
                shards.local().remove_session(session_id);
                http_sse_log.debug("SSE session {} closed", session_id);
                try { co_await out.close(); } catch (...) {}
            });

        co_return std::move(rep);
    }

    // ── POST /message 处理 ───────────────────────────────────────────────────

    static seastar::future<std::unique_ptr<seastar::http::reply>>
    _handle_message(seastar::sharded<mcp::server::McpShard>& shards,
                    std::unique_ptr<seastar::http::request> req,
                    std::unique_ptr<seastar::http::reply> rep) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        std::string body(req->content.data(), req->content.size());
#pragma GCC diagnostic pop
        std::string session_id = req->get_query_param("sessionId");

        // 在本核 dispatch JSON-RPC（传入 session_id 供 subscribe 等 handler 使用）
        auto response_opt = co_await shards.local().dispatch(body, session_id);

        if (session_id.empty()) {
            // 无 session：直接响应
            if (response_opt) rep->write_body("json", *response_opt);
            else rep->set_status(seastar::http::reply::status_type::accepted);
            co_return std::move(rep);
        }

        // 有 session：将结果 push 到 session 所属核
        if (response_opt) {
            unsigned target = _parse_shard(session_id);
            if (target == seastar::this_shard_id()) {
                // 同核，直接 push
                (void)shards.local().push_to_session(session_id, std::move(*response_opt));
            } else {
                // 跨核：通过 invoke_on 在目标核上 push
                auto msg = std::move(*response_opt);
                seastar::engine().run_in_background(
                    shards.invoke_on(target,
                        [session_id, msg = std::move(msg)](mcp::server::McpShard& s) mutable {
                            return s.push_to_session(session_id, std::move(msg));
                        }));
            }
        }
        rep->set_status(seastar::http::reply::status_type::accepted);
        co_return std::move(rep);
    }

    // 从 session ID 解析所属 shard："s{N}_{counter}" → N
    static unsigned _parse_shard(const std::string& session_id) {
        if (session_id.size() > 1 && session_id[0] == 's') {
            auto pos = session_id.find('_');
            if (pos != std::string::npos && pos > 1) {
                try { return static_cast<unsigned>(std::stoul(session_id.substr(1, pos - 1))); }
                catch (...) {}
            }
        }
        return 0;
    }
};

} // namespace mcp::transport
