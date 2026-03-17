#pragma once
#include <seastar/core/future.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/http/httpd.hh>
#include <seastar/http/handlers.hh>
#include <seastar/http/reply.hh>
#include <seastar/http/request.hh>
#include "mcp/router/dispatcher.hh"

namespace mcp::server {

    class McpHandler : public seastar::httpd::handler_base {
        mcp::router::JsonRpcDispatcher& _dispatcher;
    public:
        McpHandler(mcp::router::JsonRpcDispatcher& dispatcher) : _dispatcher(dispatcher) {}

        virtual seastar::future<std::unique_ptr<seastar::http::reply>> handle(
            const seastar::sstring& path,
            std::unique_ptr<seastar::http::request> req,
            std::unique_ptr<seastar::http::reply> rep) override 
        {
            // =========================================================
            // 【终极修复】Seastar 普通 POST 请求的 Body 存在 req->content 中
            // =========================================================
            // 1. 将 seastar::sstring 安全转换为 std::string_view
            std::string_view body_view(req->content.data(), req->content.size());
            
            // 2. 转换为标准 std::string 传给 dispatcher
            std::string body(body_view);

            // 【可选】打印出来看看，方便确认客户端发了什么
            //std::cout << "[HTTP Debug] Received body: " << body << std::endl;

            // =========================================================
            // 3. 交给 Dispatcher 处理
            // =========================================================
            auto response_opt = co_await _dispatcher.handle_request(body);

            // 4. 返回 HTTP 响应
            if (response_opt) {
                rep->write_body("json", *response_opt);
            } else {
                // 这个分支是处理 Notification (没有 id 的请求，不需要返回 JSON)
                rep->set_status(seastar::http::reply::status_type::accepted);
            }

            co_return std::move(rep);
        }
    };

    class McpServer {
    public:
        // 改回带名字的单核 http_server
        McpServer() : _server("mcp_server") {}

        seastar::future<> start(uint16_t port) {
            // 直接将路由挂载到当前核心的 server 上
            set_routes(_server._routes);
            // 启动监听
            return _server.listen(seastar::socket_address{seastar::ipv4_addr{port}});
        }

        seastar::future<> stop() {
            return _server.stop();
        }

        mcp::router::JsonRpcDispatcher& dispatcher() { return _dispatcher; }

    private:
        void set_routes(seastar::httpd::routes& r) {
            r.add(seastar::httpd::operation_type::POST, seastar::httpd::url("/message"), new McpHandler(_dispatcher));
        }

        // 关键点：去掉了 _control，变成普通的 http_server
        seastar::httpd::http_server _server;
        mcp::router::JsonRpcDispatcher _dispatcher;
    };

}