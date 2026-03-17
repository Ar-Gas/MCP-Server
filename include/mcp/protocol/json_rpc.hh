#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <optional>
#include <stdexcept>

namespace mcp::protocol {

using json = nlohmann::json;

enum class JsonRpcErrorCode : int {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603
};

class JsonRpcException : public std::runtime_error {
public:
    JsonRpcErrorCode code;
    json data;
    JsonRpcException(JsonRpcErrorCode c, const std::string& msg, json d = nullptr)
        : std::runtime_error(msg), code(c), data(std::move(d)) {}
};

struct JsonRpcError {
    int code;
    std::string message;
    json data = nullptr;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(JsonRpcError, code, message, data)

struct JsonRpcResponse {
    std::string jsonrpc = "2.0";
    json id;
    std::optional<json> result;
    std::optional<JsonRpcError> error;
};

inline void to_json(json& j, const JsonRpcResponse& r) {
    j = json{{"jsonrpc", r.jsonrpc}, {"id", r.id}};
    if (r.error.has_value()) {
        j["error"] = r.error.value();
    } else if (r.result.has_value()) {
        j["result"] = r.result.value();
    } else {
        j["result"] = nullptr;
    }
}

} // namespace mcp::protocol