#pragma once
#include "mcp/interfaces.hh"
#include <seastar/util/log.hh> // 引入 logger
#include <iostream>

namespace mcp::tools {

inline seastar::logger calc_log("tools-calculate_sum"); // 定义工具模块的日志器

class CalculateSumTool : public mcp::interfaces::McpTool {
public:
    std::string get_name() const override { return "calculate_sum"; }

    nlohmann::json get_definition() const override {
        return {
            {"name", get_name()},
            {"description", "将两个数字相加"},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"a", {{"type", "number"}, {"description", "第一个数字"}}},
                    {"b", {{"type", "number"}, {"description", "第二个数字"}}}
                }},
                {"required", nlohmann::json::array({"a", "b"})}
            }}
        };
    }

    seastar::future<nlohmann::json> execute(const nlohmann::json& args) override {
        double a = args.value("a", 0.0);
        double b = args.value("b", 0.0);
        
        calc_log.info("Executing calculate_sum: a={}, b={}", a, b);
        
        nlohmann::json result;
        result["content"] = nlohmann::json::array({{{"type", "text"}, {"text", "计算结果是: " + std::to_string(a + b)}}});
        co_return result;
    }
};

}