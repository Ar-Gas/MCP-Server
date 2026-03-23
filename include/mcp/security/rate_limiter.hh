#pragma once
#include "mcp/security/security_policy.hh"
#include <unordered_map>
#include <chrono>
#include <string>
#include <algorithm>

namespace mcp::security {

// 单个 IP 的令牌桶状态
struct TokenBucket {
    double   tokens;      // 当前令牌数
    double   capacity;    // 令牌桶容量（= burst_size）
    double   refill_per_ns; // 每纳秒补充的令牌数
    std::chrono::steady_clock::time_point last_refill;
};

// Per-IP 令牌桶限流器（每个 shard 持有独立实例，无跨核竞争）
//
// 算法：令牌桶
//   - 初始令牌数 = burst_size（允许启动时的突发流量）
//   - 每次请求消耗 1 个令牌
//   - 令牌按 requests_per_second 的速率持续补充
//   - 令牌数不超过 burst_size
//
// 注意：使用 steady_clock，适合 Seastar 单线程 shard 模型（无锁）
class PerIpRateLimiter {
    RateLimitConfig _cfg;
    std::unordered_map<std::string, TokenBucket> _buckets;

public:
    PerIpRateLimiter() : _cfg{} {}

    explicit PerIpRateLimiter(const RateLimitConfig& cfg) : _cfg(cfg) {}

    // 尝试消耗一个令牌
    // 返回 true  → 请求允许通过
    // 返回 false → 令牌耗尽，请求被限流（429）
    bool try_acquire(const std::string& ip) {
        if (!_cfg.enabled || ip.empty()) return true;

        auto& b = _get_or_create(ip);
        _refill(b);

        if (b.tokens >= 1.0) {
            b.tokens -= 1.0;
            return true;
        }
        return false;
    }

    // 重置某个 IP 的限流状态（如解封后调用）
    void reset(const std::string& ip) {
        _buckets.erase(ip);
    }

private:
    TokenBucket& _get_or_create(const std::string& ip) {
        auto it = _buckets.find(ip);
        if (it != _buckets.end()) return it->second;

        double cap  = static_cast<double>(_cfg.burst_size);
        double rate = (_cfg.requests_per_second > 0)
                    ? static_cast<double>(_cfg.requests_per_second) / 1.0e9 // per ns
                    : 0.0;
        TokenBucket b{cap, cap, rate, std::chrono::steady_clock::now()};
        return _buckets.emplace(ip, b).first->second;
    }

    void _refill(TokenBucket& b) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - b.last_refill).count();
        if (elapsed_ns > 0) {
            b.tokens = std::min(b.capacity,
                                b.tokens + static_cast<double>(elapsed_ns) * b.refill_per_ns);
            b.last_refill = now;
        }
    }
};

} // namespace mcp::security
