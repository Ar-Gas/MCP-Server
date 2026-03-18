#include "mcp/handlers/mcp_handler.hh"
#include <seastar/core/coroutine.hh>
#include <seastar/util/log.hh>

// 引入工具
#include "mcp/tools/calculate_sum_tool.hh"     
#include "mcp/tools/get_current_time_tool.hh"  

// 引入新写的资源和提示词
#include "mcp/resources/system_info_resource.hh"
#include "mcp/prompts/analyze_system_prompt.hh"

using json = nlohmann::json;

namespace mcp::handlers {

static seastar::logger handler_log("mcp_handler");

void McpHandler::register_routes(mcp::server::McpServer& server) {
    auto& dispatcher = server.dispatcher();
    auto registry = std::make_shared<McpRegistry>();
    
    // 🔥 1. 注册工具 (Tools)
    registry->register_tool(std::make_shared<mcp::tools::CalculateSumTool>());
    registry->register_tool(std::make_shared<mcp::tools::GetCurrentTimeTool>());

    // 🔥 2. 注册资源 (Resources)
    registry->register_resource(std::make_shared<mcp::resources::SystemInfoResource>());

    // 🔥 3. 注册提示词 (Prompts)
    registry->register_prompt(std::make_shared<mcp::prompts::AnalyzeSystemPrompt>());

        // =========================================================
    // 以下是协议路由分发 
    // =========================================================

    // 1. Initialize (初始化，向客户端声明我们支持的新能力)
    dispatcher.register_method("initialize", [](const json& params) -> seastar::future<json> {
        handler_log.info("Client connected and initialized.");
        json result;
        result["protocolVersion"] = "2024-11-05";
        result["serverInfo"] = {{"name", "my-seastar-mcp"}, {"version", "2.0.0"}};
        result["capabilities"] = { 
            {"tools", json::object()}, 
            {"resources", {{"listChanged", false}, {"subscribe", false}}}, 
            {"prompts", {{"listChanged", false}}} 
        };
        co_return result;
    });
    
    dispatcher.register_notification("notifications/initialized", [](const json&) -> seastar::future<> { co_return; });

    // 1.5 ping
    dispatcher.register_method("ping", [](const json&) -> seastar::future<json> {
        co_return json::object(); 
    });

    // 2. Tools (工具)
    dispatcher.register_method("tools/list", [registry](const json&) -> seastar::future<json> {
        co_return json{{"tools", registry->get_tools_list()}};
    });
    
    dispatcher.register_method("tools/call", [registry](const json& params) -> seastar::future<json> {
        std::string tool_name = params.value("name", "");
        handler_log.info("Client calling tool: {}", tool_name);
        co_return co_await registry->call_tool(tool_name, params.value("arguments", json::object()));
    });

    // 3. Resources (静态资源)
    dispatcher.register_method("resources/list", [registry](const json&) -> seastar::future<json> {
        handler_log.info("Client requested resources/list");
        co_return json{{"resources", registry->get_resources_list()}};
    });
    
    // 🔥 【新增功能 1】动态资源模板 (Resource Templates)
    dispatcher.register_method("resources/templates/list", [](const json&) -> seastar::future<json> {
        // 告诉客户端：我不光有固定的内存资源，我还可以动态查询各种组件！
        json templates = json::array({
            {
                {"uriTemplate", "sys://metrics/{component}"},
                {"name", "system_metrics_template"},
                {"description", "动态获取系统各组件的指标 (例如 component 输入 cpu 或 disk)"}
            }
        });
        co_return json{{"resourceTemplates", templates}};
    });

    dispatcher.register_method("resources/read", [registry](const json& params) -> seastar::future<json> {
        std::string uri = params.value("uri", "");
        handler_log.info("Client reading resource: {}", uri);

        // 🔥 拦截处理动态模板请求
        if (uri.find("sys://metrics/") == 0) {
            std::string comp = uri.substr(14); // 截取 {component} 部分
            // 在实际项目中，这里可以去查询数据库或底层接口
            std::string content = "动态组件 [" + comp + "] 状态: 运行良好, 负载极低。";
            co_return json{{"contents", json::array({{{"uri", uri}, {"mimeType", "text/plain"}, {"text", content}}})}};
        }

        // 如果不是模板，就去查静态注册的资源 (比如你写的 sys://memory-info)
        co_return co_await registry->read_resource(uri);
    });

    // 4. Prompts (提示词)
    dispatcher.register_method("prompts/list", [registry](const json&) -> seastar::future<json> {
        handler_log.info("Client requested prompts/list");
        co_return json{{"prompts", registry->get_prompts_list()}};
    });
    
    dispatcher.register_method("prompts/get", [registry](const json& params) -> seastar::future<json> {
        std::string prompt_name = params.value("name", "");
        handler_log.info("Client getting prompt: {}", prompt_name);
        co_return co_await registry->get_prompt(prompt_name, params.value("arguments", json::object()));
    });

    // 🔥 【新增功能 2】自动补全 (Completion) 
    dispatcher.register_method("completion/complete", [](const json& params) -> seastar::future<json> {
        handler_log.info("Client requested auto-completion");
        json ref = params.value("ref", json::object());
        json argument = params.value("argument", json::object());
        
        json values = json::array();
        
        // 当用户在客户端输入 analyze_server_health 的 focus 参数时，给予联想提示
        if (ref.value("type", "") == "prompt" && 
            ref.value("name", "") == "analyze_server_health" && 
            argument.value("name", "") == "focus") {
            
            std::string input = argument.value("value", "");
            // 简单的联想匹配：如果输入的前几个字母匹配，就返回补全建议
            if (std::string("memory").find(input) == 0) values.push_back("memory");
            if (std::string("cpu").find(input) == 0) values.push_back("cpu");
            if (std::string("disk").find(input) == 0) values.push_back("disk");
            if (std::string("network").find(input) == 0) values.push_back("network");
        }
        
        co_return json{
            {"completion", {
                {"values", values},
                {"total", values.size()},
                {"hasMore", false}
            }}
        };
    });
 }
} // namespace mcp::handlers