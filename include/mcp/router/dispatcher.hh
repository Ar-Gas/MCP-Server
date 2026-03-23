#pragma once
#include <seastar/core/future.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/util/log.hh>
#include "mcp/protocol/json_rpc.hh"
#include <unordered_map>
#include <functional>

namespace mcp::router {

using namespace mcp::protocol;
using MethodHandler       = std::function<seastar::future<json>(const json&)>;
using NotificationHandler = std::function<seastar::future<>(const json&)>;

inline seastar::logger rpc_log("rpc_dispatcher");

class JsonRpcDispatcher {
    std::unordered_map<std::string, MethodHandler>       _methods;
    std::unordered_map<std::string, NotificationHandler> _notifications;

public:
    void register_method(const std::string& method, MethodHandler handler) {
        _methods[method] = std::move(handler);
    }
    void register_notification(const std::string& method, NotificationHandler handler) {
        _notifications[method] = std::move(handler);
    }

    // 入口：自动处理单请求和 JSON-RPC Batch（数组）
    seastar::future<std::optional<std::string>> handle_request(const std::string& raw_body) {
        rpc_log.debug("Raw request: {}", raw_body);
        json req_json;
        try {
            req_json = json::parse(raw_body);
        } catch (...) {
            rpc_log.error("Failed to parse JSON");
            JsonRpcResponse err{"2.0", nullptr, nullptr,
                JsonRpcError{static_cast<int>(JsonRpcErrorCode::ParseError), "Parse error", nullptr}};
            co_return json(err).dump();
        }

        // JSON-RPC Batch：请求体为数组
        if (req_json.is_array()) {
            if (req_json.empty()) {
                JsonRpcResponse err{"2.0", nullptr, nullptr,
                    JsonRpcError{static_cast<int>(JsonRpcErrorCode::InvalidRequest),
                                 "Invalid Request: empty batch", nullptr}};
                co_return json(err).dump();
            }
            json responses = json::array();
            for (const auto& elem : req_json) {
                auto resp = co_await _handle_single(elem);
                if (resp.has_value()) {
                    try { responses.push_back(json::parse(*resp)); } catch (...) {}
                }
            }
            if (responses.empty()) co_return std::nullopt;
            co_return responses.dump();
        }

        // 单请求
        co_return co_await _handle_single(req_json);
    }

private:
    // 处理单条 JSON-RPC 请求（方法调用或通知）
    seastar::future<std::optional<std::string>> _handle_single(const json& req) {
        if (!req.is_object() ||
            !req.contains("jsonrpc") || req["jsonrpc"] != "2.0" ||
            !req.contains("method")) {
            JsonRpcResponse err{"2.0", req.value("id", json(nullptr)), nullptr,
                JsonRpcError{static_cast<int>(JsonRpcErrorCode::InvalidRequest),
                             "Invalid Request", nullptr}};
            co_return json(err).dump();
        }

        std::string method = req["method"];
        json params  = req.value("params", json::object());
        bool is_notif = !req.contains("id");
        json id = is_notif ? json(nullptr) : req["id"];

        if (is_notif) {
            auto it = _notifications.find(method);
            if (it != _notifications.end()) {
                seastar::engine().run_in_background(it->second(params));
            }
            co_return std::nullopt;
        }

        auto it = _methods.find(method);
        if (it == _methods.end()) {
            JsonRpcResponse err{"2.0", id, nullptr,
                JsonRpcError{static_cast<int>(JsonRpcErrorCode::MethodNotFound),
                             "Method not found: " + method, nullptr}};
            co_return json(err).dump();
        }

        JsonRpcResponse response{"2.0", id, nullptr, std::nullopt};
        try {
            json result = co_await it->second(params);
            response.result = std::move(result);
        } catch (const JsonRpcException& e) {
            response.error = JsonRpcError{static_cast<int>(e.code), e.what(), e.data};
        } catch (const std::exception& e) {
            response.error = JsonRpcError{static_cast<int>(JsonRpcErrorCode::InternalError),
                                          e.what(), nullptr};
        }
        co_return json(response).dump();
    }
};

} // namespace mcp::router
