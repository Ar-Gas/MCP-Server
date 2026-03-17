#include "mcp/handlers/mcp_handler.hh"
#include "mcp/tools/calculate_sum_tool.hh"
#include "mcp/tools/get_current_time_tool.hh"
#include <iostream>
#include <seastar/core/coroutine.hh>
#include <seastar/core/print.hh>
#include <stdexcept>

using json = nlohmann::json;

namespace mcp::handlers {

void McpHandler::register_routes(mcp::server::McpServer& server) {
    auto& dispatcher = server.dispatcher();

    // =========================================================
    // 0. 初始化工具注册中心 (用 shared_ptr 保证异步生命周期安全)
    // =========================================================
    auto tool_manager = std::make_shared<ToolManager>();
    
    // 🔥 在这里插上你的业务插件！
    tool_manager->register_tool(std::make_shared<mcp::tools::GetCurrentTimeTool>());
    tool_manager->register_tool(std::make_shared<mcp::tools::CalculateSumTool>());

    // =========================================================
    // 1. 初始化握手
    // =========================================================
    dispatcher.register_method("initialize", [](const json& params) -> seastar::future<json> {
        //std::cout << "\n[MCP 状态] 收到 initialize 请求\n";
        json result;
        result["protocolVersion"] = "2024-11-05";
        result["serverInfo"] = {{"name", "my-seastar-mcp"}, {"version", "1.0.0"}};
        result["capabilities"] = { {"tools", json::object()} };
        co_return result;
    });
    
    dispatcher.register_notification("notifications/initialized", [](const json& params) -> seastar::future<> {
        //std::cout << "[MCP 状态] 客户端已就绪！\n";
        co_return; 
    });

    // =========================================================
    // 2. 获取工具列表 (完全自动化！)
    // =========================================================
    dispatcher.register_method("tools/list", [tool_manager](const json& params) -> seastar::future<json> {
        //std::cout << "[MCP 工具] 客户端请求工具列表 (tools/list)\n";
        json result;
        result["tools"] = tool_manager->get_all_tools_list();
        co_return result;
    });

    // =========================================================
    // 3. 执行工具调用 (自动路由，告别 if-else！)
    // =========================================================
    dispatcher.register_method("tools/call", [tool_manager](const json& params) -> seastar::future<json> {
        std::string name = params.value("name", "");
        json args = params.value("arguments", json::object());
        
        //std::cout << "\n[MCP 工具] 客户端调用了工具: " << name << "\n";
        
        // 直接让 manager 接管执行
        co_return co_await tool_manager->call_tool(name, args);
    });
}

} // namespace mcp::handlers