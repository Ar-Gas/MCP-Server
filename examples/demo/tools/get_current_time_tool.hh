#pragma once
#include <mcp/core/interfaces.hh>
#include <seastar/util/log.hh>
#include <chrono>
#include <ctime>

namespace demo {

inline seastar::logger time_log("tools-get_current_time");

class GetCurrentTimeTool : public mcp::core::McpTool {
public:
    std::string get_name()  const override { return "get_current_time"; }
    std::string get_title() const override { return "获取当前时间"; }

    nlohmann::json get_definition() const override {
        return {
            {"name", get_name()},
            {"title", get_title()},
            {"description", "获取服务器当前的系统时间"},
            {"inputSchema", {{"type", "object"}, {"properties", nlohmann::json::object()}}}
        };
    }

    nlohmann::json get_annotations() const override {
        return {{"readOnlyHint", true}};
    }

    seastar::future<nlohmann::json> execute(const nlohmann::json&) override {
        auto now = std::chrono::system_clock::now();
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        std::string time_str = std::ctime(&now_c);
        if (!time_str.empty() && time_str.back() == '\n') time_str.pop_back();
        time_log.info("get_current_time: {}", time_str);
        nlohmann::json result;
        result["content"] = nlohmann::json::array({{{"type", "text"}, {"text", "当前服务器时间是: " + time_str}}});
        co_return result;
    }
};

} // namespace demo
