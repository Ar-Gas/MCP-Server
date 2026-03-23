#pragma once
#include "mcp/core/registry.hh"
#include "mcp/router/dispatcher.hh"
#include "mcp/transport/transport.hh"
#include "mcp/security/security_policy.hh"
#include "mcp/security/ip_filter.hh"
#include "mcp/security/rate_limiter.hh"
#include "mcp/security/circuit_breaker.hh"
#include <seastar/core/sharded.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/util/log.hh>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <string>
#include <optional>
#include <algorithm>

namespace mcp::server {

inline seastar::logger shard_log("mcp_shard");
inline seastar::logger security_audit_log("mcp_security_audit");

// McpServerConfig 在此定义，mcp_server.hh 通过 include mcp_shard.hh 获得
struct McpServerConfig {
    std::string name         = "mcp-server";
    std::string version      = "1.0.0";
    uint16_t    http_port    = 8080;
    bool        enable_stdio = false;
    bool        enable_http  = true;
    bool        enable_streamable_http = false;
    uint16_t    streamable_http_port   = 8081;

    // 安全策略：P0/P1 各项默认 disabled（size_limits 始终生效）
    // 通过 McpServerBuilder 的 with_*() 方法启用
    mcp::security::SecurityPolicy security;
};

// McpShard：每个 CPU 核心持有一个实例（通过 sharded<McpShard> 管理）
// 包含该核心独立的 dispatcher、SSE session map、订阅表、进度 token 等
class McpShard : public seastar::peering_sharded_service<McpShard> {
    using json = nlohmann::json;

    McpServerConfig _config;
    std::shared_ptr<mcp::core::McpRegistry> _registry;
    mcp::router::JsonRpcDispatcher _dispatcher;
    std::unordered_map<std::string, std::shared_ptr<mcp::transport::SseSession>> _sessions;
    uint64_t _session_counter = 0;

    // ── Phase 2a: resources/subscribe ────────────────────────────────────────
    std::string _current_session_id;
    std::unordered_map<std::string, std::unordered_set<std::string>> _subscriptions;

    // ── Phase 2b: notifications/progress ─────────────────────────────────────
    std::string _current_progress_token;

    // ── Phase 2c: notifications/message ──────────────────────────────────────
    seastar::log_level _client_log_level = seastar::log_level::warn;

    // ── Phase 3: 双向 RPC (sampling / elicitation) ───────────────────────────
    uint64_t _server_request_counter = 0;
    std::unordered_map<uint64_t, seastar::promise<json>> _pending_client_requests;

    // ── Security: 每核独立实例，无跨核竞争 ────────────────────────────────────
    mcp::security::IpFilter           _ip_filter;
    mcp::security::PerIpRateLimiter   _rate_limiter;
    mcp::security::PerIpCircuitBreaker _circuit_breaker;

    // Per-IP 连接数追踪（session_id → client_ip，ip → 连接数）
    std::unordered_map<std::string, std::string> _session_to_ip;
    std::unordered_map<std::string, size_t>      _ip_conn_count;

public:
    McpShard(McpServerConfig config, std::shared_ptr<mcp::core::McpRegistry> registry);

    seastar::future<> start() {
        // 初始化安全组件（从 config 中读取策略）
        _ip_filter       = mcp::security::IpFilter(_config.security.ip_filter);
        _rate_limiter    = mcp::security::PerIpRateLimiter(_config.security.rate_limit);
        _circuit_breaker = mcp::security::PerIpCircuitBreaker(_config.security.circuit_breaker);
        _dispatcher.set_max_batch_size(_config.security.size_limits.max_batch_size);

        _register_mcp_methods();
        shard_log.info("McpShard started on shard {}", seastar::this_shard_id());
        return seastar::make_ready_future<>();
    }

    seastar::future<> stop() {
        for (auto& [_, s] : _sessions) {
            s->active = false;
            (void)s->messages.push_eventually("");
        }
        _sessions.clear();
        for (auto& [_, p] : _pending_client_requests) {
            p.set_exception(std::make_exception_ptr(std::runtime_error("server stopped")));
        }
        _pending_client_requests.clear();
        return seastar::make_ready_future<>();
    }

    // ── Security 公开接口 ─────────────────────────────────────────────────────

    // 统一安全检查入口（transport 层在 dispatch 前调用）
    // client_ip: 点分十进制 IPv4，或空串（未知来源）
    // api_key:   从请求 header 中提取的 key 值（P2，disabled 时传空串即可）
    mcp::security::SecurityCheckResult check_request(
            const std::string& client_ip,
            const std::string& api_key = "") {
        using R = mcp::security::SecurityCheckResult;

        // 1. IP 过滤（黑名单/白名单）
        if (!_ip_filter.is_allowed(client_ip)) {
            _audit("BLOCKED_IP", client_ip, "ip_filter");
            return R::FORBIDDEN;
        }

        // 2. API Key 认证（P2，默认 disabled）
        if (_config.security.api_key.enabled) {
            const auto& keys = _config.security.api_key.keys;
            if (std::find(keys.begin(), keys.end(), api_key) == keys.end()) {
                _audit("UNAUTHORIZED", client_ip, "api_key");
                return R::UNAUTHORIZED;
            }
        }

        // 3. 速率限制（令牌桶）
        if (!_rate_limiter.try_acquire(client_ip)) {
            _audit("RATE_LIMITED", client_ip, "rate_limiter");
            return R::TOO_MANY_REQUESTS;
        }

        // 4. 熔断器
        if (!_circuit_breaker.allow(client_ip)) {
            _audit("CIRCUIT_OPEN", client_ip, "circuit_breaker");
            return R::SERVICE_UNAVAILABLE;
        }

        return R::OK;
    }

    // 请求完成后更新熔断器状态（transport 层在 dispatch 返回后调用）
    // is_rpc_error: 响应中包含 JSON-RPC "error" 字段时为 true
    void record_outcome(const std::string& client_ip, bool is_rpc_error) {
        if (is_rpc_error) {
            _circuit_breaker.record_failure(client_ip);
        } else {
            _circuit_breaker.record_success(client_ip);
        }
    }

    // 请求体大小检查（transport 层在读取 body 后立即调用）
    bool check_body_size(size_t body_size) const {
        return body_size <= _config.security.size_limits.max_body_bytes;
    }

    // ── RPC 分发入口（transport 层调用）────────────────────────────────────────
    seastar::future<std::optional<std::string>> dispatch(
            const std::string& body, const std::string& session_id = "") {
        try {
            json j = json::parse(body);
            if (j.is_object() && j.contains("id") && !j.contains("method") &&
                (j.contains("result") || j.contains("error"))) {
                if (handle_client_response(j)) {
                    co_return std::nullopt;
                }
            }
        } catch (...) {}

        _current_progress_token = "";
        _current_session_id = session_id;
        try {
            json j = json::parse(body);
            if (j.contains("params") && j["params"].contains("_meta")) {
                _current_progress_token =
                    j["params"]["_meta"].value("progressToken", "");
            }
        } catch (...) {}

        co_return co_await _dispatcher.handle_request(body);
    }

    // ── SSE session 管理 ─────────────────────────────────────────────────────

    // 创建新 session，同时追踪 client_ip 用于连接数限制
    // 返回空串表示被连接数限制拒绝
    std::string create_session(const std::string& client_ip = "") {
        if (_config.security.connection_limit.enabled) {
            if (_sessions.size() >= _config.security.connection_limit.max_total) {
                _audit("CONN_LIMIT_TOTAL", client_ip, "connection_limit");
                return "";
            }
            if (!client_ip.empty()) {
                auto it = _ip_conn_count.find(client_ip);
                size_t count = (it != _ip_conn_count.end()) ? it->second : 0;
                if (count >= _config.security.connection_limit.max_per_ip) {
                    _audit("CONN_LIMIT_PER_IP", client_ip, "connection_limit");
                    return "";
                }
            }
        }

        std::string id = "s" + std::to_string(seastar::this_shard_id())
                       + "_" + std::to_string(++_session_counter);
        _sessions[id] = std::make_shared<mcp::transport::SseSession>();
        _track_ip_conn(id, client_ip);
        return id;
    }

    void create_session_with_id(const std::string& id,
                                const std::string& client_ip = "") {
        _sessions[id] = std::make_shared<mcp::transport::SseSession>();
        _track_ip_conn(id, client_ip);
    }

    std::shared_ptr<mcp::transport::SseSession> get_session(const std::string& id) {
        auto it = _sessions.find(id);
        return it != _sessions.end() ? it->second : nullptr;
    }

    void remove_session(const std::string& id) {
        // 释放 IP 连接数计数
        auto ip_it = _session_to_ip.find(id);
        if (ip_it != _session_to_ip.end()) {
            const std::string& ip = ip_it->second;
            auto cnt_it = _ip_conn_count.find(ip);
            if (cnt_it != _ip_conn_count.end()) {
                if (cnt_it->second > 0) --cnt_it->second;
                if (cnt_it->second == 0) _ip_conn_count.erase(cnt_it);
            }
            _session_to_ip.erase(ip_it);
        }
        _sessions.erase(id);
    }

    seastar::future<> push_to_session(const std::string& id, std::string msg) {
        auto s = get_session(id);
        if (s && s->active) {
            return s->messages.push_eventually(std::move(msg));
        }
        return seastar::make_ready_future<>();
    }

    seastar::future<> broadcast_notification(std::string notification_json) {
        for (auto& [_, s] : _sessions) {
            if (s && s->active) {
                (void)s->messages.push_eventually(std::string(notification_json));
            }
        }
        return seastar::make_ready_future<>();
    }

    // ── Phase 2a: resources/subscribe ────────────────────────────────────────

    void subscribe_resource(const std::string& uri) {
        if (!_current_session_id.empty()) {
            _subscriptions[uri].insert(_current_session_id);
            shard_log.debug("Session {} subscribed to resource {}", _current_session_id, uri);
        }
    }

    void unsubscribe_resource(const std::string& uri) {
        if (!_current_session_id.empty()) {
            auto it = _subscriptions.find(uri);
            if (it != _subscriptions.end()) {
                it->second.erase(_current_session_id);
                if (it->second.empty()) _subscriptions.erase(it);
            }
        }
    }

    void cleanup_subscriptions(const std::string& session_id) {
        for (auto it = _subscriptions.begin(); it != _subscriptions.end(); ) {
            it->second.erase(session_id);
            if (it->second.empty()) it = _subscriptions.erase(it);
            else ++it;
        }
    }

    seastar::future<> notify_resource_updated(const std::string& uri) {
        auto it = _subscriptions.find(uri);
        if (it == _subscriptions.end()) co_return;
        json notification = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/resources/updated"},
            {"params", {{"uri", uri}}}
        };
        std::string msg = notification.dump();
        for (const auto& sid : it->second) {
            auto s = get_session(sid);
            if (s && s->active) {
                (void)s->messages.push_eventually(std::string(msg));
            }
        }
    }

    // ── Phase 2b: notifications/progress ─────────────────────────────────────

    seastar::future<> push_progress(float progress,
                                    std::optional<float> total = std::nullopt) {
        if (_current_session_id.empty() || _current_progress_token.empty()) {
            co_return;
        }
        json params = {
            {"progressToken", _current_progress_token},
            {"progress", progress}
        };
        if (total.has_value()) params["total"] = *total;
        json notification = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/progress"},
            {"params", std::move(params)}
        };
        co_await push_to_session(_current_session_id, notification.dump());
    }

    // ── Phase 2c: notifications/message ──────────────────────────────────────

    void set_client_log_level(seastar::log_level level) {
        _client_log_level = level;
    }

    seastar::future<> broadcast_log_notification(
            const std::string& level,
            const std::string& logger_name,
            const json& data) {
        json notification = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/message"},
            {"params", {
                {"level", level},
                {"logger", logger_name},
                {"data", data}
            }}
        };
        co_await broadcast_notification(notification.dump());
    }

    // ── Phase 3: 双向 RPC (sampling / elicitation) ───────────────────────────

    seastar::future<json> send_request_to_client(
            const std::string& session_id,
            const std::string& method,
            const json& params) {
        uint64_t req_id = ++_server_request_counter;
        json request = {
            {"jsonrpc", "2.0"},
            {"id", req_id},
            {"method", method},
            {"params", params}
        };
        seastar::promise<json> p;
        auto fut = p.get_future();
        _pending_client_requests.emplace(req_id, std::move(p));
        co_await push_to_session(session_id, request.dump());
        co_return co_await std::move(fut);
    }

    bool handle_client_response(const json& response) {
        if (!response.contains("id")) return false;
        uint64_t id;
        try {
            id = response["id"].get<uint64_t>();
        } catch (...) {
            return false;
        }
        auto it = _pending_client_requests.find(id);
        if (it == _pending_client_requests.end()) return false;
        if (response.contains("result")) {
            it->second.set_value(response["result"]);
        } else {
            auto& err = response["error"];
            it->second.set_exception(std::make_exception_ptr(
                std::runtime_error(err.value("message", "client error"))));
        }
        _pending_client_requests.erase(it);
        return true;
    }

    mcp::router::JsonRpcDispatcher& dispatcher() { return _dispatcher; }

private:
    void _register_mcp_methods();

    // 追踪 session 对应的 IP 连接数
    void _track_ip_conn(const std::string& session_id, const std::string& client_ip) {
        if (!client_ip.empty()) {
            _session_to_ip[session_id] = client_ip;
            ++_ip_conn_count[client_ip];
        }
    }

    // 安全审计日志（只在 enable_audit_log = true 时输出）
    void _audit(const std::string& event, const std::string& ip,
                const std::string& component = "") {
        if (_config.security.enable_audit_log) {
            if (component.empty()) {
                security_audit_log.info("[AUDIT] shard={} event={} ip={}",
                    seastar::this_shard_id(), event, ip.empty() ? "unknown" : ip);
            } else {
                security_audit_log.info("[AUDIT] shard={} event={} ip={} component={}",
                    seastar::this_shard_id(), event, ip.empty() ? "unknown" : ip, component);
            }
        }
    }
};

} // namespace mcp::server
