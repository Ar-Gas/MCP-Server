#pragma once
#include <seastar/core/future.hh>
#include <seastar/core/coroutine.hh>
#include <nlohmann/json.hpp>
#include <string>

namespace mcp::tools {

class McpTool {
public:
    virtual ~McpTool() = default;

    // 1. 工具的名称
    virtual std::string get_name() const = 0;

    // 2. 工具的 JSON 描述（用于生成 tools/list）
    virtual nlohmann::json get_definition() const = 0;

    // 3. 核心业务逻辑实现
    virtual seastar::future<nlohmann::json> execute(const nlohmann::json& args) = 0;
};

} // namespace mcp::tools