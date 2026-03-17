#include <seastar/core/posix.hh>    // <--- 新增这行，为了引入全局 handle_signal
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <iostream>
#include <csignal>
#include "mcp/server/mcp_server.hh"
#include "mcp/handlers/mcp_handler.hh" // 引入业务逻辑头文件

int main(int argc, char** argv) {
    seastar::app_template app;
    
    return app.run(argc, argv, []() -> seastar::future<> {
        mcp::server::McpServer server;
        
        // 【关键】一行代码，将所有的业务路由注册到 Server 中！
        mcp::handlers::McpHandler::register_routes(server);
        
        // 启动服务器
        co_await server.start(8080);
        std::cout << "MCP Server is running on port 8080. Press Ctrl+C to stop.\n";

        // 处理停止信号
        seastar::promise<> stop_signal;
        bool stopped = false;
        auto stop_handler = [&stop_signal, &stopped] {
            if (!stopped) { stopped = true; stop_signal.set_value(); }
        };
        seastar::engine().handle_signal(SIGINT, [stop_handler] { stop_handler(); }); // 捕获 Ctrl+C 信号
        seastar::engine().handle_signal(SIGTERM, [stop_handler] { stop_handler(); }); // 捕获终止信号

        co_await stop_signal.get_future();
        std::cout << "\nShutting down MCP Server...\n";
        co_await server.stop();
    });
}