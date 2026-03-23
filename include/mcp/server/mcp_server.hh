#pragma once
#include "mcp/server/mcp_shard.hh"
#include "mcp/transport/transport.hh"
#include <seastar/core/future.hh>
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>

namespace mcp::server {

// McpServer：SDK 对外暴露的服务器主类
// 内部通过 sharded<McpShard> 实现真正的多核并行
// 通过 McpServerBuilder（见 core/builder.hh）创建
class McpServer {
    McpServerConfig _config;
    std::shared_ptr<mcp::core::McpRegistry> _registry;
    seastar::sharded<McpShard> _shards;
    std::vector<std::unique_ptr<mcp::transport::ITransport>> _transports;

public:
    McpServer(McpServerConfig config, std::shared_ptr<mcp::core::McpRegistry> registry)
        : _config(std::move(config)), _registry(std::move(registry)) {}

    void add_transport(std::unique_ptr<mcp::transport::ITransport> t) {
        _transports.push_back(std::move(t));
    }

    seastar::future<> start();
    seastar::future<> stop();

    seastar::sharded<McpShard>& shards()   { return _shards; }
    mcp::core::McpRegistry&     registry() { return *_registry; }
    const McpServerConfig&      config()   { return _config; }

    // ── Phase 2a: 全核广播 resource 更新（SDK 用户在 resource 变化时调用）─────
    seastar::future<> broadcast_resource_updated(const std::string& uri) {
        return _shards.invoke_on_all([uri](McpShard& s) {
            return s.notify_resource_updated(uri);
        });
    }

    // ── Phase 2c: 全核广播日志通知（SDK 用户工具内调用）──────────────────────
    seastar::future<> broadcast_log_notification(
            const std::string& level,
            const std::string& logger_name,
            const nlohmann::json& data) {
        return _shards.invoke_on_all([level, logger_name, data](McpShard& s) {
            return s.broadcast_log_notification(level, logger_name, data);
        });
    }

    // ── Phase 3: 向客户端发起 sampling / elicitation 请求 ────────────────────
    // 必须在 session 所属核上调用（session_id 编码了 shard）
    seastar::future<nlohmann::json> request_sampling(
            const std::string& session_id,
            const nlohmann::json& params) {
        return _shards.local().send_request_to_client(
            session_id, "sampling/createMessage", params);
    }

    seastar::future<nlohmann::json> request_elicitation(
            const std::string& session_id,
            const nlohmann::json& params) {
        return _shards.local().send_request_to_client(
            session_id, "elicitation/create", params);
    }
};

} // namespace mcp::server

