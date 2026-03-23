#pragma once
#include <seastar/core/future.hh>
#include <seastar/core/queue.hh>

// 前向声明，避免与 mcp_server.hh 的循环 include
namespace mcp::server { class McpServer; }

namespace mcp::transport {

// SSE 长连接会话（由所属 shard 的 McpShard 持有）
struct SseSession {
    seastar::queue<std::string> messages{100};
    bool active = true;
};

// ITransport：传输层抽象接口
// start() 接受 McpServer&，可通过 server.shards() 访问多核分片状态
class ITransport {
public:
    virtual ~ITransport() = default;
    virtual seastar::future<> start(mcp::server::McpServer& server) = 0;
    virtual seastar::future<> stop() = 0;
};

} // namespace mcp::transport
