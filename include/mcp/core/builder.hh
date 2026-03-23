#pragma once
#include "mcp/core/interfaces.hh"
#include "mcp/core/registry.hh"
#include "mcp/server/mcp_server.hh"
#include <memory>
#include <string>
#include <vector>

namespace mcp {

// McpServerBuilder：流式 API，配置并构建 McpServer
//
// ── 使用示例 ───────────────────────────────────────────────────────────────
//
// 基础用法（P0 默认生效：1MB body 限制 + 20条 batch 限制）：
//   McpServerBuilder().name("my-server").with_http(8080).build()
//
// 启用 P0/P1 安全防护：
//   McpServerBuilder()
//     .with_ip_whitelist({"10.0.0.0/8", "192.168.0.0/16"})  // 仅内网
//     .with_rate_limit(100, 200)          // 每IP 100 req/s，突发 200
//     .with_circuit_breaker(5, 30)        // 连续5次失败熔断，30s 后半开
//     .with_connection_limit(10, 5000)    // 每IP最多10个SSE连接，总共5000
//     .with_audit_log()                   // 开启安全审计日志
//     .with_http(8080).build()
//
// P2 可选安全（程序员显式启用）：
//   .with_api_key({"secret-key-1", "secret-key-2"})  // API Key 认证
//   .with_tls("cert.pem", "key.pem")                 // TLS（config ready，transport 待实现）
//   .with_tls("cert.pem", "key.pem", "ca.pem")       // mTLS（双向认证）
class McpServerBuilder {
    server::McpServerConfig _config;
    std::shared_ptr<core::McpRegistry> _registry;

public:
    McpServerBuilder() : _registry(std::make_shared<core::McpRegistry>()) {}

    // ── 基础配置 ───────────────────────────────────────────────────────────

    McpServerBuilder& name(std::string n) {
        _config.name = std::move(n);
        return *this;
    }
    McpServerBuilder& version(std::string v) {
        _config.version = std::move(v);
        return *this;
    }
    McpServerBuilder& with_http(uint16_t port = 8080) {
        _config.enable_http = true;
        _config.http_port = port;
        return *this;
    }
    McpServerBuilder& with_stdio() {
        _config.enable_stdio = true;
        return *this;
    }
    McpServerBuilder& with_streamable_http(uint16_t port = 8081) {
        _config.enable_streamable_http = true;
        _config.streamable_http_port = port;
        return *this;
    }

    // ── Tool / Resource / Prompt 注册 ──────────────────────────────────────

    template<typename T, typename... Args>
    McpServerBuilder& add_tool(Args&&... args) {
        _registry->register_tool(std::make_shared<T>(std::forward<Args>(args)...));
        return *this;
    }

    template<typename T, typename... Args>
    McpServerBuilder& add_resource(Args&&... args) {
        _registry->register_resource(std::make_shared<T>(std::forward<Args>(args)...));
        return *this;
    }

    template<typename T, typename... Args>
    McpServerBuilder& add_prompt(Args&&... args) {
        _registry->register_prompt(std::make_shared<T>(std::forward<Args>(args)...));
        return *this;
    }

    // ── P0: IP 过滤 ────────────────────────────────────────────────────────
    //
    // 白名单：只有列表内的 CIDR 段可以访问，其余全部拒绝（403）
    // 黑名单：列表内的 CIDR 段直接拒绝，不影响其他 IP（可与白名单同时使用）
    // 黑名单优先于白名单
    //
    // 示例：
    //   .with_ip_whitelist({"10.0.0.0/8"})          // 仅内网 10.x.x.x
    //   .with_ip_blacklist({"192.168.1.100/32"})     // 屏蔽特定主机

    McpServerBuilder& with_ip_whitelist(std::vector<std::string> cidrs) {
        _config.security.ip_filter.whitelist = std::move(cidrs);
        _config.security.ip_filter.default_allow = false;  // 白名单模式：非白即拒
        return *this;
    }

    McpServerBuilder& with_ip_blacklist(std::vector<std::string> cidrs) {
        _config.security.ip_filter.blacklist = std::move(cidrs);
        return *this;
    }

    // ── P0: Per-IP 速率限制（令牌桶）──────────────────────────────────────
    //
    // requests_per_second: 稳态每秒最大请求数
    // burst_size:          允许的突发请求数（0 = 自动设为 rps * 2）
    // 超限时返回 429 Too Many Requests
    //
    // 示例：
    //   .with_rate_limit(50)         // 每IP 50 req/s，突发 100
    //   .with_rate_limit(100, 500)   // 每IP 100 req/s，突发 500

    McpServerBuilder& with_rate_limit(uint32_t requests_per_second,
                                       uint32_t burst_size = 0) {
        _config.security.rate_limit.enabled = true;
        _config.security.rate_limit.requests_per_second = requests_per_second;
        _config.security.rate_limit.burst_size =
            (burst_size > 0) ? burst_size : requests_per_second * 2;
        return *this;
    }

    // ── P0: Per-IP 熔断器 ──────────────────────────────────────────────────
    //
    // 当某个 IP 连续产生 failure_threshold 次 JSON-RPC error 响应时，
    // 该 IP 的熔断器打开（OPEN），拒绝后续请求（503）。
    // 等待 timeout_seconds 秒后转入 HALF_OPEN，允许少量探测请求。
    // 探测成功 success_threshold 次后恢复为 CLOSED。
    //
    // 示例：
    //   .with_circuit_breaker()          // 默认：5次失败，30s 恢复
    //   .with_circuit_breaker(10, 60)    // 10次失败，60s 恢复

    McpServerBuilder& with_circuit_breaker(uint32_t failure_threshold = 5,
                                            uint32_t timeout_seconds = 30,
                                            uint32_t success_threshold = 2) {
        _config.security.circuit_breaker.enabled = true;
        _config.security.circuit_breaker.failure_threshold = failure_threshold;
        _config.security.circuit_breaker.timeout_seconds   = timeout_seconds;
        _config.security.circuit_breaker.success_threshold = success_threshold;
        return *this;
    }

    // ── P1: SSE 连接数限制 ─────────────────────────────────────────────────
    //
    // max_per_ip:  单个 IP 最大并发 SSE 连接数（超出返回 503）
    // max_total:   全服务器最大并发 SSE 连接数（超出返回 503）
    //
    // 示例：
    //   .with_connection_limit()           // 默认：每IP 10，总共 10000
    //   .with_connection_limit(5, 1000)    // 每IP 5，总共 1000

    McpServerBuilder& with_connection_limit(size_t max_per_ip = 10,
                                             size_t max_total = 10000) {
        _config.security.connection_limit.enabled    = true;
        _config.security.connection_limit.max_per_ip = max_per_ip;
        _config.security.connection_limit.max_total  = max_total;
        return *this;
    }

    // ── P0: 请求体 & Batch 大小限制（默认已启用，可通过此方法调整） ───────────
    //
    // max_body_mb:  请求体最大大小（MB，默认 1MB）
    // max_batch:    JSON-RPC Batch 最大条数（默认 20）

    McpServerBuilder& with_size_limits(size_t max_body_mb = 1,
                                        uint32_t max_batch = 20) {
        _config.security.size_limits.max_body_bytes = max_body_mb * 1024 * 1024;
        _config.security.size_limits.max_batch_size = max_batch;
        return *this;
    }

    // ── P1: 安全审计日志 ───────────────────────────────────────────────────
    //
    // 开启后，所有被拒绝的请求（IP 过滤、速率限制、熔断、连接数超限）
    // 将写入 logger "mcp_security_audit"，格式：
    //   [AUDIT] shard=N event=BLOCKED_IP ip=x.x.x.x component=ip_filter
    //
    // 使用 --logger-log-level mcp_security_audit=info 查看

    McpServerBuilder& with_audit_log(bool enable = true) {
        _config.security.enable_audit_log = enable;
        return *this;
    }

    // ── P2: API Key / Bearer Token 认证（可选，默认 disabled）─────────────
    //
    // 启用后，每个请求必须携带有效的 API Key，否则返回 401 Unauthorized。
    // 支持两种方式（自动识别）：
    //   Header: X-API-Key: <your-key>
    //   Header: Authorization: Bearer <your-key>
    //
    // 示例：
    //   .with_api_key({"key-abc123", "key-xyz789"})
    //   .with_api_key({"my-token"}, "Authorization")  // 仅接受 Authorization header

    McpServerBuilder& with_api_key(std::vector<std::string> keys,
                                    std::string header = "X-API-Key") {
        _config.security.api_key.enabled = true;
        _config.security.api_key.keys    = std::move(keys);
        _config.security.api_key.header  = std::move(header);
        return *this;
    }

    // ── P2: TLS / mTLS（可选，config 就绪，transport 层待后续实现）──────────
    //
    // 单向 TLS：
    //   .with_tls("server.crt", "server.key")
    //
    // 双向 mTLS（客户端证书认证）：
    //   .with_tls("server.crt", "server.key", "ca.crt")
    //
    // 注意：当前 transport 层 TLS 支持尚未完成，配置项已保留，
    //       后续版本将与 Seastar TLS 集成。

    McpServerBuilder& with_tls(std::string cert_pem, std::string key_pem,
                                std::string ca_pem = "") {
        _config.security.tls.enabled  = true;
        _config.security.tls.cert_pem = std::move(cert_pem);
        _config.security.tls.key_pem  = std::move(key_pem);
        _config.security.tls.ca_pem   = std::move(ca_pem);
        return *this;
    }

    // ── 构建 ──────────────────────────────────────────────────────────────

    std::unique_ptr<server::McpServer> build() {
        return std::make_unique<server::McpServer>(std::move(_config), std::move(_registry));
    }
};

} // namespace mcp
