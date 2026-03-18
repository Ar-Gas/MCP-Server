#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh> // 必须有这个才能使用 seastar::engine()
#include <seastar/core/sharded.hh>
#include <iostream>
#include <thread>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <csignal>
#include "mcp/server/mcp_server.hh"
#include "mcp/handlers/mcp_handler.hh"

void run_stdio_bridge(uint16_t port) {
    std::string line;
    // 不断从标准输入读取，直到文件结束或程序退出
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue;

        sockaddr_in serv_addr{};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);
        
        if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) { 
            close(sock); 
            continue; 
        }
        
        // 【修复点】：加上 Content-Type 和 Connection: close
        // 如果你的真实路由是 /message，请把下面的 "POST / " 改回 "POST /message "
        std::string req = "POST /message HTTP/1.1\r\n"
                          "Host: 127.0.0.1\r\n"
                          "Content-Type: application/json\r\n"
                          "Connection: close\r\n" // 这一行极其重要！
                          "Content-Length: " + std::to_string(line.size()) + "\r\n\r\n" + 
                          line;
                          
        send(sock, req.c_str(), req.size(), 0);
        
        std::string resp;
        char buffer[4096];
        // 因为加了 Connection: close，Seastar 发完数据会主动断开 TCP，read 就会返回 0 结束循环
        while (true) {
            int bytes = read(sock, buffer, sizeof(buffer));
            if (bytes <= 0) break;
            resp.append(buffer, bytes);
        }
        close(sock);
        
        // 提取 HTTP Body
        auto body_pos = resp.find("\r\n\r\n");
        if (body_pos != std::string::npos) {
            // MCP 协议要求输出必须换行，这里用 \n 是对的
            std::cout << resp.substr(body_pos + 4) << "\n";
            std::cout.flush(); // 确保立刻输出，不被缓冲
        }
    }
}

int main(int argc, char** argv) {
    seastar::app_template app;
    return app.run(argc, argv, []() -> seastar::future<> {
        uint16_t listen_port = 8080;
        
        // --- 修复点：去掉 static，让 server 在 co_await server.stop() 后销毁 ---
        auto server = std::make_unique<mcp::server::McpServer>(); 
        
        mcp::handlers::McpHandler::register_routes(*server);
        
        co_await server->start(listen_port);
        // 启动后台线程处理 Stdio
        std::thread(run_stdio_bridge, listen_port).detach();
        std::cerr << "[INFO] Seastar MCP Server is running on port " << listen_port << "\n";
        seastar::promise<> stop_signal;
        seastar::engine().handle_signal(SIGINT, [&] { stop_signal.set_value(); });
        seastar::engine().handle_signal(SIGTERM, [&] { stop_signal.set_value(); });
        co_await stop_signal.get_future();
        
        std::cerr << "Shutting down...\n";
        co_await server->stop();
        
        // 显式销毁 server，确保在引擎停止前析构
        server.reset();
        std::cerr << "Done. Exiting.\n";
        // 强制退出，防止 detached 线程在 std::cin 阻塞时产生冲突
        std::exit(0); 
    });
}