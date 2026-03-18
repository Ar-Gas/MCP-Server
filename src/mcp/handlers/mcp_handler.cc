#include "mcp/handlers/mcp_handler.hh"
#include <seastar/core/coroutine.hh>
#include <seastar/util/log.hh> // 引入 Seastar 高性能日志
#include "mcp/tools/calculate_sum_tool.hh"     // 引入你的真实工具
#include "mcp/tools/get_current_time_tool.hh"  // 引入你的真实工具

using json = nlohmann::json;

namespace mcp::handlers {

// 定义一个名为 "mcp_handler" 的专属异步日志器
static seastar::logger handler_log("mcp_handler");

// -------- Dummy 实现，为了演示 Resources 和 Prompts --------
class DummyResource : public mcp::interfaces::McpResource {
    std::string get_uri() const override { return "file:///logs/system.log"; }
    std::string get_name() const override { return "system_logs"; }
    json get_definition() const override { return {{"uri", get_uri()}, {"name", get_name()}, {"mimeType", "text/plain"}}; }
    seastar::future<std::string> read() override { co_return "System OK. Seastar running at 10000 QPS."; }
};

class DummyPrompt : public mcp::interfaces::McpPrompt {
    std::string get_name() const override { return "analyze_logs"; }
    json get_definition() const override { return {{"name", get_name()}, {"description", "Analyze the given logs"}}; }
    seastar::future<json> get_messages(const json& args) override {
        json msgs = json::array();
        msgs.push_back({{"role", "user"}, {"content", {{"type", "text"}, {"text", "Please analyze these logs: ..."}}}});
        co_return msgs;
    }
};
// -------------------------------------------------------------

void McpHandler::register_routes(mcp::server::McpServer& server) {
    auto& dispatcher = server.dispatcher();
    auto registry = std::make_shared<McpRegistry>();
    
    // 🔥 注册你写的真实工具 🔥
    registry->register_tool(std::make_shared<mcp::tools::CalculateSumTool>());
    registry->register_tool(std::make_shared<mcp::tools::GetCurrentTimeTool>());

    // 注册资源和提示词
    registry->register_resource(std::make_shared<DummyResource>());
    registry->register_prompt(std::make_shared<DummyPrompt>());

    // 1. Initialize
    dispatcher.register_method("initialize", [](const json& params) -> seastar::future<json> {
        handler_log.info("Client connected and initialized. Protocol: 2024-11-05");
        json result;
        result["protocolVersion"] = "2024-11-05";
        result["serverInfo"] = {{"name", "my-seastar-mcp"}, {"version", "2.0.0"}};
        result["capabilities"] = { 
            {"tools", json::object()}, 
            {"resources", json::object()}, 
            {"prompts", json::object()} 
        };
        co_return result;
    });
    
    dispatcher.register_notification("notifications/initialized", [](const json&) -> seastar::future<> { co_return; });

    // 2. Tools
    dispatcher.register_method("tools/list", [registry](const json&) -> seastar::future<json> {
        handler_log.info("Client requested tools/list");
        co_return json{{"tools", registry->get_tools_list()}};
    });
    
    dispatcher.register_method("tools/call", [registry](const json& params) -> seastar::future<json> {
        std::string tool_name = params.value("name", "");
        handler_log.info("Client calling tool: {}", tool_name); // 打印调用日志
        co_return co_await registry->call_tool(tool_name, params.value("arguments", json::object()));
    });

    // 3. Resources
    dispatcher.register_method("resources/list", [registry](const json&) -> seastar::future<json> {
        co_return json{{"resources", registry->get_resources_list()}};
    });
    dispatcher.register_method("resources/read", [registry](const json& params) -> seastar::future<json> {
        co_return co_await registry->read_resource(params.value("uri", ""));
    });

    // 4. Prompts
    dispatcher.register_method("prompts/list", [registry](const json&) -> seastar::future<json> {
        co_return json{{"prompts", registry->get_prompts_list()}};
    });
    dispatcher.register_method("prompts/get", [registry](const json& params) -> seastar::future<json> {
        co_return co_await registry->get_prompt(params.value("name", ""), params.value("arguments", json::object()));
    });
}

} // namespace mcp::handlers