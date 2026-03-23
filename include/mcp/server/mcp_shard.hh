#pragma once
#include "mcp/core/registry.hh"
#include "mcp/router/dispatcher.hh"
#include "mcp/transport/transport.hh"
#include <seastar/core/sharded.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/util/log.hh>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <string>
#include <optional>

namespace mcp::server {

inline seastar::logger shard_log("mcp_shard");

// McpServerConfig 在此定义，mcp_server.hh 通过 include mcp_shard.hh 获得
struct McpServerConfig {
    std::string name         = "mcp-server";
    std::string version      = "1.0.0";
    uint16_t    http_port    = 8080;
    bool        enable_stdio = false;
    bool        enable_http  = true;
    bool        enable_streamable_http = false;
    uint16_t    streamable_http_port   = 8081;
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
    // 当前正在处理的请求所属 session（仅供同步 handler 使用，无 co_await）
    std::string _current_session_id;
    // uri → 订阅了该 uri 的 session_id 集合（本核）
    std::unordered_map<std::string, std::unordered_set<std::string>> _subscriptions;

    // ── Phase 2b: notifications/progress ─────────────────────────────────────
    std::string _current_progress_token;

    // ── Phase 2c: notifications/message ──────────────────────────────────────
    seastar::log_level _client_log_level = seastar::log_level::warn;

    // ── Phase 3: 双向 RPC (sampling / elicitation) ───────────────────────────
    uint64_t _server_request_counter = 0;
    std::unordered_map<uint64_t, seastar::promise<json>> _pending_client_requests;

public:
    McpShard(McpServerConfig config, std::shared_ptr<mcp::core::McpRegistry> registry);

    seastar::future<> start() {
        _register_mcp_methods();
        shard_log.info("McpShard started on shard {}", seastar::this_shard_id());
        return seastar::make_ready_future<>();
    }

    seastar::future<> stop() {
        // 关闭所有 SSE session，通知 streaming 协程退出
        for (auto& [_, s] : _sessions) {
            s->active = false;
            (void)s->messages.push_eventually("");
        }
        _sessions.clear();
        // 取消所有待处理的双向 RPC 请求
        for (auto& [_, p] : _pending_client_requests) {
            p.set_exception(std::make_exception_ptr(std::runtime_error("server stopped")));
        }
        _pending_client_requests.clear();
        return seastar::make_ready_future<>();
    }

    // ── RPC 分发入口（transport 层调用）────────────────────────────────────────
    // session_id：发起本次请求的 SSE session（空串表示无 session，如 StdIO）
    seastar::future<std::optional<std::string>> dispatch(
            const std::string& body, const std::string& session_id = "") {
        // 先检测是否为客户端对服务端请求的响应（Phase 3）
        try {
            json j = json::parse(body);
            if (j.is_object() && j.contains("id") && !j.contains("method") &&
                (j.contains("result") || j.contains("error"))) {
                if (handle_client_response(j)) {
                    co_return std::nullopt; // 已由双向 RPC 处理
                }
            }
        } catch (...) {}

        // 提取 _meta.progressToken（Phase 2b）
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

    std::string create_session() {
        std::string id = "s" + std::to_string(seastar::this_shard_id())
                       + "_" + std::to_string(++_session_counter);
        _sessions[id] = std::make_shared<mcp::transport::SseSession>();
        return id;
    }

    void create_session_with_id(const std::string& id) {
        _sessions[id] = std::make_shared<mcp::transport::SseSession>();
    }

    std::shared_ptr<mcp::transport::SseSession> get_session(const std::string& id) {
        auto it = _sessions.find(id);
        return it != _sessions.end() ? it->second : nullptr;
    }

    void remove_session(const std::string& id) {
        _sessions.erase(id);
    }

    seastar::future<> push_to_session(const std::string& id, std::string msg) {
        auto s = get_session(id);
        if (s && s->active) {
            return s->messages.push_eventually(std::move(msg));
        }
        return seastar::make_ready_future<>();
    }

    // 广播服务器通知到本核所有活跃 SSE session
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

    // 断连时清理该 session 的所有订阅
    void cleanup_subscriptions(const std::string& session_id) {
        for (auto it = _subscriptions.begin(); it != _subscriptions.end(); ) {
            it->second.erase(session_id);
            if (it->second.empty()) it = _subscriptions.erase(it);
            else ++it;
        }
    }

    // 向本核所有订阅了 uri 的 session 推送 notifications/resources/updated
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

    // 向当前 session 推送进度通知（工具 handler 内调用）
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

    // 向本核所有活跃 session 推送日志通知（高于 _client_log_level 才推送）
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

    // 向客户端发送 JSON-RPC 请求，通过 SSE 推送，等待客户端通过 POST 返回响应
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

    // transport 调用：处理客户端对服务端请求的响应，返回 true 表示已处理
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
};

} // namespace mcp::server
