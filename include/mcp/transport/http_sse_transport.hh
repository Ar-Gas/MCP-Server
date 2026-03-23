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
//
// 安全防护（在每个请求最前端执行）：
//   P0: 请求体大小限制（max_body_bytes）
//   P0: IP 黑名单/白名单过滤
//   P0: Per-IP 速率限制（令牌桶）
//   P0: Per-IP 熔断器（CLOSED/OPEN/HALF_OPEN）
//   P1: SSE 连接数限制（max_per_ip / max_total）
//   P2: API Key 认证（disabled by default）
class HttpSseTransport : public ITransport {
    uint16_t _port;
    seastar::httpd::http_server_control _server;

public:
    explicit HttpSseTransport(uint16_t port = 8080) : _port(port) {}

    seastar::future<> start(mcp::server::McpServer& server) override {
        co_await _server.start("mcp_http_sse");
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

    // ── 安全辅助方法 ──────────────────────────────────────────────────────────

    // 从请求中提取客户端 IP
    // 优先读 X-Real-IP（反向代理设置），其次 X-Forwarded-For 的第一个 IP，
    // 最后返回空串（由 IpFilter::is_allowed 按 default_allow 策略处理）
    static std::string _get_client_ip(const seastar::http::request& req) {
        auto real_ip = req.get_header("x-real-ip");
        if (!real_ip.empty()) return std::string(real_ip);

        auto forwarded = req.get_header("x-forwarded-for");
        if (!forwarded.empty()) {
            std::string fwd = forwarded;
            auto pos = fwd.find(',');
            auto ip = (pos != std::string::npos) ? fwd.substr(0, pos) : fwd;
            // 去除前后空格
            while (!ip.empty() && ip.front() == ' ') ip.erase(ip.begin());
            while (!ip.empty() && ip.back()  == ' ') ip.pop_back();
            return ip;
        }
        return "";  // 直连但无法获取 IP（由 default_allow 决定）
    }

    // 从请求 header 中提取 API Key（P2）
    // 支持 "X-API-Key: <key>" 或 "Authorization: Bearer <token>"
    static std::string _get_api_key(const seastar::http::request& req) {
        auto key = req.get_header("x-api-key");
        if (!key.empty()) return std::string(key);

        auto auth = req.get_header("authorization");
        if (auth.size() > 7 && auth.substr(0, 7) == "Bearer ") {
            return std::string(auth.substr(7));
        }
        return "";
    }

    // 将 SecurityCheckResult 转换为 HTTP 错误响应
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

    // ── SSE 连接处理 ─────────────────────────────────────────────────────────

    static seastar::future<std::unique_ptr<seastar::http::reply>>
    _handle_sse(seastar::sharded<mcp::server::McpShard>& shards,
                std::unique_ptr<seastar::http::request> req,
                std::unique_ptr<seastar::http::reply> rep) {
        std::string client_ip = _get_client_ip(*req);
        std::string api_key   = _get_api_key(*req);

        // P0/P1/P2 安全检查（IP 过滤 + 速率限制 + 熔断器 + API Key）
        auto check = shards.local().check_request(client_ip, api_key);
        if (check != mcp::security::SecurityCheckResult::OK) {
            co_return co_await _security_reject(std::move(rep), check);
        }

        // P1: 连接数限制（create_session 内部检查，返回空串表示拒绝）
        std::string session_id = shards.local().create_session(client_ip);
        if (session_id.empty()) {
            co_return co_await _security_reject(std::move(rep),
                mcp::security::SecurityCheckResult::CONNECTION_LIMIT);
        }

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
        std::string client_ip = _get_client_ip(*req);
        std::string api_key   = _get_api_key(*req);

        // P0: 请求体大小检查（最先执行，防止大包消耗内存）
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

        std::string session_id = req->get_query_param("sessionId");

        // dispatch JSON-RPC（传入 session_id 供 subscribe 等 handler 使用）
        auto response_opt = co_await shards.local().dispatch(body, session_id);

        // 更新熔断器状态
        bool is_error = response_opt &&
            response_opt->find("\"error\"") != std::string::npos;
        shards.local().record_outcome(client_ip, is_error);

        if (session_id.empty()) {
            if (response_opt) rep->write_body("json", *response_opt);
            else rep->set_status(seastar::http::reply::status_type::accepted);
            co_return std::move(rep);
        }

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
