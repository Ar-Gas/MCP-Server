#pragma once
#include "mcp/transport/transport.hh"
#include "mcp/server/mcp_shard.hh"
#include "mcp/server/mcp_server.hh"
#include "mcp/security/security_policy.hh"
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
// 安全防护：与 HttpSseTransport 相同的 P0/P1/P2 检查体系
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
    struct _PostMcpHandler : seastar::httpd::handler_base {
        seastar::sharded<mcp::server::McpShard>& _shards;
        explicit _PostMcpHandler(seastar::sharded<mcp::server::McpShard>& s) : _shards(s) {}
        seastar::future<std::unique_ptr<seastar::http::reply>> handle(
            const seastar::sstring&,
            std::unique_ptr<seastar::http::request> req,
            std::unique_ptr<seastar::http::reply> rep) override {
            return _handle_post(_shards, std::move(req), std::move(rep))
                .then([](std::unique_ptr<seastar::http::reply> r) {
                    r->done();
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

    // ── 安全辅助方法（与 HttpSseTransport 一致）──────────────────────────────

    static std::string _get_client_ip(const seastar::http::request& req) {
        auto real_ip = req.get_header("x-real-ip");
        if (!real_ip.empty()) return std::string(real_ip);

        auto forwarded = req.get_header("x-forwarded-for");
        if (!forwarded.empty()) {
            std::string fwd = forwarded;
            auto pos = fwd.find(',');
            auto ip = (pos != std::string::npos) ? fwd.substr(0, pos) : fwd;
            while (!ip.empty() && ip.front() == ' ') ip.erase(ip.begin());
            while (!ip.empty() && ip.back()  == ' ') ip.pop_back();
            return ip;
        }
        return "";
    }

    static std::string _get_api_key(const seastar::http::request& req) {
        auto key = req.get_header("x-api-key");
        if (!key.empty()) return std::string(key);

        auto auth = req.get_header("authorization");
        if (auth.size() > 7 && auth.substr(0, 7) == "Bearer ") {
            return std::string(auth.substr(7));
        }
        return "";
    }

    static seastar::future<std::unique_ptr<seastar::http::reply>>
    _security_reject(std::unique_ptr<seastar::http::reply> rep,
                     mcp::security::SecurityCheckResult result) {
        using R = mcp::security::SecurityCheckResult;
        using S = seastar::http::reply::status_type;

        switch (result) {
        case R::FORBIDDEN:
            rep->set_status(S::forbidden);
            rep->write_body("json", R"({"error":"Forbidden","code":403})");
            break;
        case R::UNAUTHORIZED:
            rep->set_status(S::unauthorized);
            rep->add_header("WWW-Authenticate", "Bearer");
            rep->write_body("json", R"({"error":"Unauthorized","code":401})");
            break;
        case R::TOO_MANY_REQUESTS:
            rep->set_status(static_cast<S>(429));
            rep->add_header("Retry-After", "1");
            rep->write_body("json", R"({"error":"Too Many Requests","code":429})");
            break;
        case R::SERVICE_UNAVAILABLE:
        case R::CONNECTION_LIMIT:
            rep->set_status(S::service_unavailable);
            rep->write_body("json", R"({"error":"Service Unavailable","code":503})");
            break;
        case R::PAYLOAD_TOO_LARGE:
            rep->set_status(static_cast<S>(413));
            rep->write_body("json", R"({"error":"Payload Too Large","code":413})");
            break;
        default:
            break;
        }
        co_return std::move(rep);
    }

    // ── POST /mcp ────────────────────────────────────────────────────────────

    static seastar::future<std::unique_ptr<seastar::http::reply>>
    _handle_post(seastar::sharded<mcp::server::McpShard>& shards,
                 std::unique_ptr<seastar::http::request> req,
                 std::unique_ptr<seastar::http::reply> rep) {
        std::string client_ip = _get_client_ip(*req);
        std::string api_key   = _get_api_key(*req);

        // P0: 请求体大小检查
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        std::string body(req->content.data(), req->content.size());
#pragma GCC diagnostic pop

        if (!shards.local().check_body_size(body.size())) {
            co_return co_await _security_reject(std::move(rep),
                mcp::security::SecurityCheckResult::PAYLOAD_TOO_LARGE);
        }

        // P0/P1/P2 安全检查
        auto check = shards.local().check_request(client_ip, api_key);
        if (check != mcp::security::SecurityCheckResult::OK) {
            co_return co_await _security_reject(std::move(rep), check);
        }

        std::string session_id = req->get_header("mcp-session-id");
        std::string accept     = req->get_header("accept");
        bool wants_sse = (accept.find("text/event-stream") != std::string::npos);

        if (session_id.empty() && !wants_sse) {
            // ── 无会话，简单请求/响应模式 ────────────────────────────────
            auto response_opt = co_await shards.local().dispatch(body, "");
            bool is_error = response_opt &&
                response_opt->find("\"error\"") != std::string::npos;
            shards.local().record_outcome(client_ip, is_error);

            if (response_opt) {
                rep->write_body("json", *response_opt);
            } else {
                rep->set_status(seastar::http::reply::status_type::accepted);
            }
            co_return std::move(rep);
        }

        if (session_id.empty() && wants_sse) {
            // ── 新建 SSE session（P1: 连接数限制）────────────────────────
            session_id = "sm" + std::to_string(seastar::this_shard_id())
                       + "_" + std::to_string(_next_counter());
            shards.local().create_session_with_id(session_id, client_ip);
            auto session = shards.local().get_session(session_id);
            if (!session) {
                co_return co_await _security_reject(std::move(rep),
                    mcp::security::SecurityCheckResult::CONNECTION_LIMIT);
            }

            rep->add_header("mcp-session-id", session_id);
            rep->set_content_type("text/event-stream");
            rep->add_header("Cache-Control", "no-cache");
            rep->add_header("Connection", "keep-alive");

            auto first_resp = co_await shards.local().dispatch(body, session_id);
            bool is_error = first_resp &&
                first_resp->find("\"error\"") != std::string::npos;
            shards.local().record_outcome(client_ip, is_error);
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
        bool is_error = response_opt &&
            response_opt->find("\"error\"") != std::string::npos;
        shards.local().record_outcome(client_ip, is_error);

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
        std::string client_ip = _get_client_ip(*req);
        std::string api_key   = _get_api_key(*req);

        // P0/P1/P2 安全检查（GET 不读 body，跳过 body size 检查）
        auto check = shards.local().check_request(client_ip, api_key);
        if (check != mcp::security::SecurityCheckResult::OK) {
            co_return co_await _security_reject(std::move(rep), check);
        }

        std::string session_id = req->get_header("mcp-session-id");
        if (session_id.empty()) {
            rep->set_status(seastar::http::reply::status_type::bad_request);
            rep->write_body("json", R"({"error":"Mcp-Session-Id header required"})");
            co_return std::move(rep);
        }

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
        // DELETE 不需要安全检查（已有 session 的持有者才能删除）
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
        std::size_t start = (id.size() > 2 && id[0] == 's' && id[1] == 'm') ? 2 : 1;
        auto pos = id.find('_', start);
        if (pos != std::string::npos && pos > start) {
            try { return static_cast<unsigned>(std::stoul(id.substr(start, pos - start))); }
            catch (...) {}
        }
        return 0;
    }

    static uint64_t _next_counter() {
        static thread_local uint64_t counter = 0;
        return ++counter;
    }
};

} // namespace mcp::transport
