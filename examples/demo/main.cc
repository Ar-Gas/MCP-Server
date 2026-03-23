#include <mcp/mcp.hh>
#include "tools/calculate_sum_tool.hh"
#include "tools/get_current_time_tool.hh"
#include "resources/system_info_resource.hh"
#include "prompts/analyze_system_prompt.hh"

#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/signal.hh>
#include <csignal>
#include <iostream>

int main(int argc, char** argv) {
    seastar::app_template app;
    return app.run(argc, argv, []() -> seastar::future<> {
        auto server = mcp::McpServerBuilder{}
            .name("seastar-mcp-demo")
            .version("2.0.0")
            .with_http(8080)
            .with_streamable_http(8081)
            .with_stdio()
            .add_tool<demo::CalculateSumTool>()
            .add_tool<demo::GetCurrentTimeTool>()
            .add_resource<demo::SystemInfoResource>()
            .add_prompt<demo::AnalyzeSystemPrompt>()
            .build();

        co_await server->start();
        std::cerr << "[INFO] seastar-mcp-demo running (HTTP/SSE :8080, Streamable HTTP :8081, StdIO)\n";

        seastar::promise<> stop_signal;
        seastar::handle_signal(SIGINT,  [&] { stop_signal.set_value(); }, true);
        seastar::handle_signal(SIGTERM, [&] { stop_signal.set_value(); }, true);
        co_await stop_signal.get_future();

        std::cerr << "[INFO] Shutting down...\n";
        co_await server->stop();
    });
}
