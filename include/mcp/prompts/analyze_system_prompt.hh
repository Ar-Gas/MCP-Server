#pragma once
#include "mcp/interfaces.hh"
#include <nlohmann/json.hpp>

namespace mcp::prompts {

class AnalyzeSystemPrompt : public mcp::interfaces::McpPrompt {
public:
    std::string get_name() const override { return "analyze_server_health"; }

    nlohmann::json get_definition() const override {
        return {
            {"name", get_name()},
            {"description", "请求 AI 助手分析当前 Seastar 服务器的健康状态和性能瓶颈"},
            {"arguments", nlohmann::json::array({
                {
                    {"name", "focus"},
                    {"description", "分析重点：可选 'memory'(内存) 或 'cpu'(处理器)"},
                    {"required", false}
                }
            })}
        };
    }

    seastar::future<nlohmann::json> get_messages(const nlohmann::json& args) override {
        std::string focus = args.value("focus", "general");
        
        // 构建发给大模型的系统提示词
        std::string prompt_text = 
            "你是一个高级 C++ 和 Seastar 框架性能调优专家。\n"
            "请分析当前服务器的健康状态。特别关注重点为：[" + focus + "]。\n"
            "操作步骤：\n"
            "1. 请先读取资源 uri: 'sys://memory-info' 获取实时内存和核心数据。\n"
            "2. 根据数据，评估是否存在内存泄漏风险，以及分配策略是否健康。\n"
            "3. 给出优化建议。";

        nlohmann::json msgs = nlohmann::json::array();
        msgs.push_back({
            {"role", "user"},
            {"content", {
                {"type", "text"},
                {"text", prompt_text}
            }}
        });

        co_return msgs;
    }
};

} // namespace mcp::prompts