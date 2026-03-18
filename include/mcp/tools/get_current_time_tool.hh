#pragma once
#include "mcp/interfaces.hh"
#include <chrono>
#include <ctime>
#include <iostream>

namespace mcp::tools {

inline seastar::logger time_log("tools-get_current_time"); // 定义工具模块的日志器

class GetCurrentTimeTool : public mcp::interfaces::McpTool {
public:
    std::string get_name() const override { return "get_current_time"; }

    nlohmann::json get_definition() const override {
        return {
            {"name", get_name()},
            {"description", "获取服务器当前的系统时间"},
            {"inputSchema", {{"type", "object"}, {"properties", nlohmann::json::object()}}}
        };
    }

    seastar::future<nlohmann::json> execute(const nlohmann::json& args) override {
        auto now = std::chrono::system_clock::now();
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        std::string time_str = std::ctime(&now_c);
        if (!time_str.empty() && time_str.back() == '\n') time_str.pop_back();
        
        time_log.info("Executing get_current_time: {}", time_str);
        
        nlohmann::json result;
        result["content"] = nlohmann::json::array({{{"type", "text"}, {"text", "当前服务器时间是: " + time_str}}});
        co_return result;
    }
};

}