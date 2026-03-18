#pragma once
#include <seastar/core/future.hh>
#include <seastar/core/coroutine.hh>
#include <nlohmann/json.hpp>
#include <string>

namespace mcp::interfaces {

// ================== 1. Tool 接口 ==================
class McpTool {
public:
    virtual ~McpTool() = default;
    virtual std::string get_name() const = 0;
    virtual nlohmann::json get_definition() const = 0;
    virtual seastar::future<nlohmann::json> execute(const nlohmann::json& args) = 0;
};

// ================== 2. Resource 接口 ==================
class McpResource {
public:
    virtual ~McpResource() = default;
    virtual std::string get_uri() const = 0;
    virtual std::string get_name() const = 0;
    virtual nlohmann::json get_definition() const = 0;
    virtual seastar::future<std::string> read() = 0;
};

// ================== 3. Prompt 接口 ==================
class McpPrompt {
public:
    virtual ~McpPrompt() = default;
    virtual std::string get_name() const = 0;
    virtual nlohmann::json get_definition() const = 0;
    virtual seastar::future<nlohmann::json> get_messages(const nlohmann::json& args) = 0;
};

} // namespace mcp::interfaces