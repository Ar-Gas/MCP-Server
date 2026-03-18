#pragma once
#include <seastar/core/future.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/util/log.hh>  // 加入头文件
#include "mcp/protocol/json_rpc.hh"
#include <unordered_map>
#include <functional>
namespace mcp::router {
using namespace mcp::protocol;
using MethodHandler = std::function<seastar::future<json>(const json&)>;
using NotificationHandler = std::function<seastar::future<>(const json&)>;
// 专门给 RPC 调度器用的 logger
inline seastar::logger rpc_log("rpc_dispatcher");
class JsonRpcDispatcher {
private:
    std::unordered_map<std::string, MethodHandler> _methods;
    std::unordered_map<std::string, NotificationHandler> _notifications;

public:
    void register_method(const std::string& method, MethodHandler handler) {
        _methods[method] = std::move(handler);
    }

    void register_notification(const std::string& method, NotificationHandler handler) {
        _notifications[method] = std::move(handler);
    }

    seastar::future<std::optional<std::string>> handle_request(const std::string& raw_body) {
        rpc_log.debug("Raw request received: {}", raw_body);
        json req_json;
        try {
            req_json = json::parse(raw_body);
        } catch (...) {
            rpc_log.error("Failed to parse JSON request");
            JsonRpcResponse err{"2.0", nullptr, nullptr, 
                JsonRpcError{static_cast<int>(JsonRpcErrorCode::ParseError), "Parse error", nullptr}};
            co_return json(err).dump();
        }

        if (!req_json.contains("jsonrpc") || req_json["jsonrpc"] != "2.0" || !req_json.contains("method")) {
            JsonRpcResponse err{"2.0", req_json.value("id", json(nullptr)), nullptr, 
                JsonRpcError{static_cast<int>(JsonRpcErrorCode::InvalidRequest), "Invalid Request", nullptr}};
            co_return json(err).dump();
        }

        std::string method = req_json["method"];
        json params = req_json.value("params", json::object());
        bool is_notification = !req_json.contains("id");
        json id = is_notification ? json(nullptr) : req_json["id"];

        if (is_notification) {
            auto it = _notifications.find(method);
            if (it != _notifications.end()) {
                (void)it->second(params); // Fire and forget in Seastar context
            }
            co_return std::nullopt;
        }

        auto it = _methods.find(method);
        if (it == _methods.end()) {
            JsonRpcResponse err{"2.0", id, nullptr, 
                JsonRpcError{static_cast<int>(JsonRpcErrorCode::MethodNotFound), "Method not found", nullptr}};
            co_return json(err).dump();
        }

        JsonRpcResponse response{"2.0", id, nullptr, std::nullopt};
        try {
            json result = co_await it->second(params);
            response.result = std::move(result);
        } catch (const JsonRpcException& e) {
            response.error = JsonRpcError{static_cast<int>(e.code), e.what(), e.data};
        } catch (const std::exception& e) {
            response.error = JsonRpcError{static_cast<int>(JsonRpcErrorCode::InternalError), e.what(), nullptr};
        }

        co_return json(response).dump();
    }
};

} // namespace mcp::router