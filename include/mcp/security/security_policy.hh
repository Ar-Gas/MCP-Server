#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace mcp::security {

// ── P0/P1: always-available config ────────────────────────────────────────────

// IP 白名单/黑名单配置（CIDR 格式，如 "192.168.0.0/24"）
struct IpFilterConfig {
    std::vector<std::string> whitelist;   // 白名单 CIDR 列表（为空则不限制）
    std::vector<std::string> blacklist;   // 黑名单 CIDR 列表（优先于白名单）
    bool default_allow = true;            // whitelist 为空时是否默认放行
};

// Per-IP 令牌桶限流配置
struct RateLimitConfig {
    bool     enabled             = false;
    uint32_t requests_per_second = 100;   // 每秒允许的请求数
    uint32_t burst_size          = 200;   // 突发容量（令牌桶上限）
};

// Per-IP 熔断器配置：连续失败 → OPEN → 等待 → HALF_OPEN → 探测 → CLOSED
struct CircuitBreakerConfig {
    bool     enabled            = false;
    uint32_t failure_threshold  = 5;    // 连续失败几次触发熔断
    uint32_t success_threshold  = 2;    // HALF_OPEN 状态需要几次成功才恢复
    uint32_t timeout_seconds    = 30;   // OPEN 状态持续时间（秒）后转 HALF_OPEN
    uint32_t half_open_max_reqs = 3;    // HALF_OPEN 时允许的最大探测请求数
};

// 并发连接数限制
struct ConnectionLimitConfig {
    bool   enabled     = false;
    size_t max_per_ip  = 10;      // 单个 IP 最大并发 SSE 连接数
    size_t max_total   = 10000;   // 全局最大并发 SSE 连接数
};

// 请求体与 Batch 大小限制（P0 默认启用）
struct SizeLimitConfig {
    size_t   max_body_bytes = 1 * 1024 * 1024;  // 请求体最大字节数（默认 1 MB）
    uint32_t max_batch_size = 20;               // JSON-RPC Batch 最大条数
};

// ── P2: 可选高级安全（默认 disabled，程序员按需启用）──────────────────────────

// TLS / mTLS 配置（transport 层尚未实现，配置项保留供后续扩展）
struct TlsConfig {
    bool        enabled  = false;
    std::string cert_pem;   // 证书文件路径
    std::string key_pem;    // 私钥文件路径
    std::string ca_pem;     // CA 证书路径（mTLS 双向认证时填写）
};

// API Key / Bearer Token 认证配置
struct ApiKeyConfig {
    bool                     enabled = false;
    std::vector<std::string> keys;                  // 合法的 key 列表
    std::string              header  = "X-API-Key"; // 优先检查的 header 名
    // 同时支持 "Authorization: Bearer <token>"
};

// ── 安全检查结果 ───────────────────────────────────────────────────────────────

enum class SecurityCheckResult {
    OK,                   // 通过，正常处理
    FORBIDDEN,            // 403 - IP 被黑名单/白名单拒绝
    TOO_MANY_REQUESTS,    // 429 - 超出速率限制
    SERVICE_UNAVAILABLE,  // 503 - 熔断器已打开
    UNAUTHORIZED,         // 401 - API Key 验证失败（P2）
    CONNECTION_LIMIT,     // 503 - 连接数超限
    PAYLOAD_TOO_LARGE,    // 413 - 请求体超限
};

// ── 聚合安全策略（加入 McpServerConfig）──────────────────────────────────────

struct SecurityPolicy {
    // P0 - 默认启用（size_limits 始终生效）
    SizeLimitConfig       size_limits;

    // P0/P1 - 默认 disabled，通过 Builder 显式开启
    IpFilterConfig        ip_filter;
    RateLimitConfig       rate_limit;
    CircuitBreakerConfig  circuit_breaker;
    ConnectionLimitConfig connection_limit;

    // P1 - 审计日志（false by default）
    bool enable_audit_log = false;

    // ── P2: 高级安全，disabled by default ─────────────────────────────────────
    // 程序员可通过 builder.with_api_key(...) / builder.with_tls(...) 开启
    ApiKeyConfig api_key;
    TlsConfig    tls;  // 配置项已就绪，transport 层 TLS 支持待后续实现
};

} // namespace mcp::security
