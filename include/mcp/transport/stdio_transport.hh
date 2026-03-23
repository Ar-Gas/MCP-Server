#pragma once
#include "mcp/transport/transport.hh"
#include "mcp/server/mcp_server.hh"
#include <seastar/core/thread.hh>
#include <seastar/util/log.hh>
#include <iostream>
#include <optional>

namespace mcp::transport {

inline seastar::logger stdio_log("stdio_transport");

// StdioTransport：基于 seastar::thread 的 stdin/stdout 传输
//
// seastar::thread 是 Seastar 的协程纤程（不是 std::thread），
// 内部可调用 future.get() 而不阻塞 reactor，是处理阻塞式 stdin 的正确模式。
// StdIO 始终在 shard 0 上运行（stdin/stdout 是全局资源）。
class StdioTransport : public ITransport {
    std::optional<seastar::thread> _thread;
    bool _stop_requested = false;

public:
    seastar::future<> start(mcp::server::McpServer& server) override {
        stdio_log.info("StdioTransport starting on shard 0");
        _thread.emplace([this, &shards = server.shards()]() {
            std::string line;
            while (!_stop_requested && std::getline(std::cin, line)) {
                if (line.empty()) continue;
                // shards.local() 在 shard 0 的 seastar::thread 中始终指向 shard 0 的 McpShard
                auto result = shards.local().dispatch(line, "").get();
                if (result.has_value()) {
                    std::cout << *result << "\n";
                    std::cout.flush();
                }
            }
            stdio_log.info("StdioTransport stdin closed");
        });
        return seastar::make_ready_future<>();
    }

    seastar::future<> stop() override {
        _stop_requested = true;
        if (_thread.has_value()) {
            return _thread->join().then([this] {
                _thread.reset();
            });
        }
        return seastar::make_ready_future<>();
    }
};

} // namespace mcp::transport
