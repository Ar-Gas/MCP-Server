#pragma once
#include <mcp/core/interfaces.hh>
#include <seastar/util/log.hh>

namespace demo {

inline seastar::logger calc_log("tools-calculate_sum");

class CalculateSumTool : public mcp::core::McpTool {
public:
    std::string get_name()  const override { return "calculate_sum"; }
    std::string get_title() const override { return "计算两数之和"; }

    nlohmann::json get_definition() const override {
        return {
            {"name", get_name()},
            {"title", get_title()},
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

    // 演示 Structured Tool Output：声明 outputSchema
    std::optional<nlohmann::json> get_output_schema() const override {
        return nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"sum", {{"type", "number"}, {"description", "两数之和"}}}
            }},
            {"required", nlohmann::json::array({"sum"})}
        };
    }

    // 演示 Tool Annotations
    nlohmann::json get_annotations() const override {
        return {{"readOnlyHint", true}, {"idempotentHint", true}};
    }

    seastar::future<nlohmann::json> execute(const nlohmann::json& args) override {
        double a = args.value("a", 0.0);
        double b = args.value("b", 0.0);
        calc_log.info("calculate_sum: a={}, b={}", a, b);
        nlohmann::json result;
        result["content"] = nlohmann::json::array({{{"type", "text"}, {"text", "计算结果是: " + std::to_string(a + b)}}});
        co_return result;
    }
};

} // namespace demo
