#pragma once
#include <seastar/core/future.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/queue.hh>
#include <seastar/http/httpd.hh>
#include <seastar/http/handlers.hh>
#include <seastar/http/function_handlers.hh>
#include "mcp/router/dispatcher.hh"

namespace mcp::server {

    struct SseSession {
        seastar::queue<std::string> messages{100};
        bool active = true;
    };

    class McpServer {
        seastar::httpd::http_server _server;
        mcp::router::JsonRpcDispatcher _dispatcher;
        std::unordered_map<std::string, std::shared_ptr<SseSession>> _sessions;
        uint64_t _session_counter = 0;

    public:
        McpServer() : _server("mcp_server") {}
        mcp::router::JsonRpcDispatcher& dispatcher() { return _dispatcher; }

        seastar::future<> start(uint16_t port) {
            set_routes(_server._routes);
            return _server.listen(seastar::socket_address{seastar::ipv4_addr{port}});
        }
        seastar::future<> stop() { return _server.stop(); }

    private:
        void set_routes(seastar::httpd::routes& r) {
            // 修复点 1: function_handler 需要第二个参数指定默认 content-type
            r.add(seastar::httpd::operation_type::GET, seastar::httpd::url("/sse"), 
                new seastar::httpd::function_handler([this](std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep) {
                    return handle_sse(std::move(req), std::move(rep));
                }, "txt")); 
            
            r.add(seastar::httpd::operation_type::POST, seastar::httpd::url("/message"), 
                new seastar::httpd::function_handler([this](std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep) {
                    return handle_message(std::move(req), std::move(rep));
                }, "json"));
        }

        seastar::future<std::unique_ptr<seastar::http::reply>> handle_sse(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep) {
            std::string session_id = "sess_" + std::to_string(++_session_counter);
            auto session = std::make_shared<SseSession>();
            _sessions[session_id] = session;

            rep->set_content_type("text/event-stream");
            rep->add_header("Cache-Control", "no-cache");
            rep->add_header("Connection", "keep-alive");

            rep->write_body("text/event-stream", [session_id, session](seastar::output_stream<char>&& out) mutable -> seastar::future<> {
                try {
                    co_await out.write("event: endpoint\ndata: /message?sessionId=" + session_id + "\n\n");
                    co_await out.flush();
                    while (session->active) {
                        auto msg = co_await session->messages.pop_eventually();
                        if (msg.empty() || !session->active) break;
                        co_await out.write("data: " + msg + "\n\n");
                        co_await out.flush();
                    }
                } catch (...) { session->active = false; }
                co_await out.close();
            });
            co_return std::move(rep);
        }

        seastar::future<std::unique_ptr<seastar::http::reply>> handle_message(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep) {
            
// 👇 开启编译器魔法：忽略废弃警告 (对 GCC 和 Clang 均有效)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            // 底层已经将流读取完毕，数据一定在 content 中，直接取用
            std::string body(req->content.data(), req->content.size());
#pragma GCC diagnostic pop
// 👆 恢复编译器警告设置
            std::string session_id = req->get_query_param("sessionId");
            
            auto response_opt = co_await _dispatcher.handle_request(body);
            if (!session_id.empty() && _sessions.contains(session_id)) {
                if (response_opt) {
                    (void)_sessions[session_id]->messages.push_eventually(std::move(*response_opt));
                }
                rep->set_status(seastar::http::reply::status_type::accepted);
            } else {
                if (response_opt) rep->write_body("json", *response_opt);
                else rep->set_status(seastar::http::reply::status_type::accepted);
            }
            co_return std::move(rep);
        }
    };
}