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

inline seastar::logger streamable_http_log("streamable_http_transport");

// StreamableHttpTransport：MCP 2024-11-05 Streamable HTTP Transport
//
// 单端点 /mcp，同时支持请求/响应模式与 SSE 流模式：
//
// POST /mcp（无 Mcp-Session-Id）：
//   - 直接 dispatch，返回 application/json
//
// POST /mcp（有 Accept: text/event-stream，初始化 session）：
//   - 创建 SSE session，Mcp-Session-Id 写入响应 header
//   - dispatch 当前请求，结果作为第一条 SSE event 发送
//   - 连接保持，后续服务端推送通过同一 SSE 流发送
//
// POST /mcp（有 Mcp-Session-Id，后续请求）：
//   - dispatch，把结果 push 到已有 SSE session
//   - 响应 202 Accepted
//
// GET /mcp（有 Mcp-Session-Id）：
//   - 直接挂载到已有 session 的 SSE 流（用于重连）
//
// DELETE /mcp（有 Mcp-Session-Id）：
//   - 关闭并清理 session，响应 200
//
// Session ID 格式：sm{shard_id}_{counter}（区别于 SSE transport 的 s{N}_{counter}）
class StreamableHttpTransport : public ITransport {
    uint16_t _port;
    seastar::httpd::http_server_control _server;

public:
    explicit StreamableHttpTransport(uint16_t port = 8081) : _port(port) {}

    seastar::future<> start(mcp::server::McpServer& server) override {
        co_await _server.start("mcp_streamable_http");
        co_await _server.set_routes([&shards = server.shards()](seastar::httpd::routes& r) {
            _setup_routes(r, shards);
        });
        streamable_http_log.info("StreamableHttpTransport listening on port {}", _port);
        co_await _server.listen(seastar::socket_address{seastar::ipv4_addr{_port}});
    }

    seastar::future<> stop() override {
        return _server.stop();
    }

private:
    // ── Custom handler: preserves Content-Type set by _handle_post ──────────
    //
    // seastar::httpd::function_handler always calls rep->done(_type) after the
    // user function returns, unconditionally overwriting the Content-Type header.
    // For POST /mcp we serve both application/json and text/event-stream
    // depending on the Accept header, so we need to preserve what _handle_post set.
    // This handler calls done() (no-arg, only sets _response_line) instead.
    struct _PostMcpHandler : seastar::httpd::handler_base {
        seastar::sharded<mcp::server::McpShard>& _shards;
        explicit _PostMcpHandler(seastar::sharded<mcp::server::McpShard>& s) : _shards(s) {}
        seastar::future<std::unique_ptr<seastar::http::reply>> handle(
            const seastar::sstring&,
            std::unique_ptr<seastar::http::request> req,
            std::unique_ptr<seastar::http::reply> rep) override {
            return _handle_post(_shards, std::move(req), std::move(rep))
                .then([](std::unique_ptr<seastar::http::reply> r) {
                    r->done();  // sets _response_line only, does NOT override Content-Type
                    return seastar::make_ready_future<std::unique_ptr<seastar::http::reply>>(
                        std::move(r));
                });
        }
    };

    static void _setup_routes(seastar::httpd::routes& r,
                               seastar::sharded<mcp::server::McpShard>& shards) {
        r.add(seastar::httpd::operation_type::POST, seastar::httpd::url("/mcp"),
            new _PostMcpHandler(shards));

        r.add(seastar::httpd::operation_type::GET, seastar::httpd::url("/mcp"),
            new seastar::httpd::function_handler(
                [&shards](std::unique_ptr<seastar::http::request> req,
                          std::unique_ptr<seastar::http::reply> rep)
                    -> seastar::future<std::unique_ptr<seastar::http::reply>> {
                    return _handle_get(shards, std::move(req), std::move(rep));
                }, "txt"));

        r.add(seastar::httpd::operation_type::DELETE, seastar::httpd::url("/mcp"),
            new seastar::httpd::function_handler(
                [&shards](std::unique_ptr<seastar::http::request> req,
                          std::unique_ptr<seastar::http::reply> rep)
                    -> seastar::future<std::unique_ptr<seastar::http::reply>> {
                    return _handle_delete(shards, std::move(req), std::move(rep));
                }, "json"));
    }

    // ── POST /mcp ────────────────────────────────────────────────────────────

    static seastar::future<std::unique_ptr<seastar::http::reply>>
    _handle_post(seastar::sharded<mcp::server::McpShard>& shards,
                 std::unique_ptr<seastar::http::request> req,
                 std::unique_ptr<seastar::http::reply> rep) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        std::string body(req->content.data(), req->content.size());
#pragma GCC diagnostic pop
        std::string session_id = req->get_header("mcp-session-id");
        std::string accept     = req->get_header("accept");
        bool wants_sse = (accept.find("text/event-stream") != std::string::npos);

        if (session_id.empty() && !wants_sse) {
            // ── 无会话，简单请求/响应模式 ────────────────────────────────
            auto response_opt = co_await shards.local().dispatch(body, "");
            if (response_opt) {
                rep->write_body("json", *response_opt);
            } else {
                rep->set_status(seastar::http::reply::status_type::accepted);
            }
            co_return std::move(rep);
        }

        if (session_id.empty() && wants_sse) {
            // ── 新建 SSE session（携带 Accept: text/event-stream）────────
            session_id = "sm" + std::to_string(seastar::this_shard_id())
                       + "_" + std::to_string(_next_counter());
            shards.local().create_session_with_id(session_id);
            auto session = shards.local().get_session(session_id);

            rep->add_header("mcp-session-id", session_id);
            rep->set_content_type("text/event-stream");
            rep->add_header("Cache-Control", "no-cache");
            rep->add_header("Connection", "keep-alive");

            // 先 dispatch 当前请求，结果作为第一条 SSE event 发送
            auto first_resp = co_await shards.local().dispatch(body, session_id);
            if (first_resp) {
                (void)session->messages.push_eventually(std::move(*first_resp));
            }

            rep->write_body("text/event-stream",
                [&shards, session_id, session](seastar::output_stream<char> out) mutable
                    -> seastar::future<> {
                    try {
                        while (session->active) {
                            auto msg = co_await session->messages.pop_eventually();
                            if (msg.empty() || !session->active) break;
                            co_await out.write("data: " + msg + "\n\n");
                            co_await out.flush();
                        }
                    } catch (...) {
                        session->active = false;
                    }
                    shards.local().cleanup_subscriptions(session_id);
                    shards.local().remove_session(session_id);
                    streamable_http_log.debug("Streamable SSE session {} closed", session_id);
                    try { co_await out.close(); } catch (...) {}
                });

            co_return std::move(rep);
        }

        // ── 已有 session：dispatch 并 push ───────────────────────────────
        auto response_opt = co_await shards.local().dispatch(body, session_id);
        if (response_opt) {
            unsigned target = _parse_shard(session_id);
            if (target == seastar::this_shard_id()) {
                (void)shards.local().push_to_session(session_id, std::move(*response_opt));
            } else {
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

    // ── GET /mcp（重连已有 SSE session）─────────────────────────────────────

    static seastar::future<std::unique_ptr<seastar::http::reply>>
    _handle_get(seastar::sharded<mcp::server::McpShard>& shards,
                std::unique_ptr<seastar::http::request> req,
                std::unique_ptr<seastar::http::reply> rep) {
        std::string session_id = req->get_header("mcp-session-id");
        if (session_id.empty()) {
            rep->set_status(seastar::http::reply::status_type::bad_request);
            rep->write_body("json", R"({"error":"Mcp-Session-Id header required"})");
            co_return std::move(rep);
        }

        // 找到 session 所属核，但 SSE 流必须在本核处理
        // 如果 session 归属其他核，客户端应重连到对应核（暂返回404）
        auto session = shards.local().get_session(session_id);
        if (!session) {
            rep->set_status(seastar::http::reply::status_type::not_found);
            rep->write_body("json", R"({"error":"Session not found on this shard"})");
            co_return std::move(rep);
        }

        rep->set_content_type("text/event-stream");
        rep->add_header("Cache-Control", "no-cache");
        rep->add_header("Connection", "keep-alive");
        rep->add_header("mcp-session-id", session_id);

        rep->write_body("text/event-stream",
            [&shards, session_id, session](seastar::output_stream<char> out) mutable
                -> seastar::future<> {
                try {
                    while (session->active) {
                        auto msg = co_await session->messages.pop_eventually();
                        if (msg.empty() || !session->active) break;
                        co_await out.write("data: " + msg + "\n\n");
                        co_await out.flush();
                    }
                } catch (...) {
                    session->active = false;
                }
                shards.local().remove_session(session_id);
                streamable_http_log.debug("Streamable GET session {} closed", session_id);
                try { co_await out.close(); } catch (...) {}
            });

        co_return std::move(rep);
    }

    // ── DELETE /mcp（关闭 session）──────────────────────────────────────────

    static seastar::future<std::unique_ptr<seastar::http::reply>>
    _handle_delete(seastar::sharded<mcp::server::McpShard>& shards,
                   std::unique_ptr<seastar::http::request> req,
                   std::unique_ptr<seastar::http::reply> rep) {
        std::string session_id = req->get_header("mcp-session-id");
        if (!session_id.empty()) {
            unsigned target = _parse_shard(session_id);
            if (target == seastar::this_shard_id()) {
                auto s = shards.local().get_session(session_id);
                if (s) {
                    s->active = false;
                    (void)s->messages.push_eventually("");
                }
                shards.local().remove_session(session_id);
            } else {
                seastar::engine().run_in_background(
                    shards.invoke_on(target,
                        [session_id](mcp::server::McpShard& shard) -> seastar::future<> {
                            auto s = shard.get_session(session_id);
                            if (s) {
                                s->active = false;
                                (void)s->messages.push_eventually("");
                            }
                            shard.remove_session(session_id);
                            return seastar::make_ready_future<>();
                        }));
            }
        }
        rep->set_status(seastar::http::reply::status_type::ok);
        co_return std::move(rep);
    }

    // 从 session ID 解析 shard："sm{N}_{counter}" → N
    static unsigned _parse_shard(const std::string& id) {
        // 支持 "sm{N}_..." 和 "s{N}_..."（兼容 SSE transport 格式）
        std::size_t start = (id.size() > 2 && id[0] == 's' && id[1] == 'm') ? 2 : 1;
        auto pos = id.find('_', start);
        if (pos != std::string::npos && pos > start) {
            try { return static_cast<unsigned>(std::stoul(id.substr(start, pos - start))); }
            catch (...) {}
        }
        return 0;
    }

    // 原子计数器（每核独立，配合 shard ID 保证全局唯一）
    static uint64_t _next_counter() {
        static thread_local uint64_t counter = 0;
        return ++counter;
    }
};

} // namespace mcp::transport
