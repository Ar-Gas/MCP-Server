#pragma once
#include "mcp/security/security_policy.hh"
#include <unordered_map>
#include <chrono>
#include <string>

namespace mcp::security {

// 熔断器状态
enum class CbState {
    CLOSED,     // 正常，放行所有请求
    OPEN,       // 熔断，拒绝所有请求（等待 timeout_seconds 后转 HALF_OPEN）
    HALF_OPEN,  // 探测，放行少量请求检验服务是否恢复
};

// 单个 IP 的熔断状态
struct CircuitState {
    CbState  state          = CbState::CLOSED;
    uint32_t failure_count  = 0;   // CLOSED 状态下的连续失败次数
    uint32_t success_count  = 0;   // HALF_OPEN 状态下的连续成功次数
    uint32_t half_open_reqs = 0;   // HALF_OPEN 状态下已发出的探测请求数
    std::chrono::steady_clock::time_point open_time;  // 进入 OPEN 的时间点
};

// Per-IP 熔断器（每个 shard 持有独立实例，无跨核竞争）
//
// 状态机：
//   CLOSED ──[连续失败 ≥ failure_threshold]──▶ OPEN
//   OPEN   ──[等待 timeout_seconds]──────────▶ HALF_OPEN
//   HALF_OPEN ──[连续成功 ≥ success_threshold]──▶ CLOSED
//   HALF_OPEN ──[任意失败]──────────────────────▶ OPEN（重置计时器）
//
// 触发熔断的"失败"定义：
//   transport 层在 dispatch() 返回后，检查响应是否包含 JSON-RPC error，
//   如果是则调用 record_failure(ip)，否则调用 record_success(ip)
class PerIpCircuitBreaker {
    CircuitBreakerConfig _cfg;
    std::unordered_map<std::string, CircuitState> _states;

public:
    PerIpCircuitBreaker() : _cfg{} {}

    explicit PerIpCircuitBreaker(const CircuitBreakerConfig& cfg) : _cfg(cfg) {}

    // 检查该 IP 是否允许发起请求
    // 返回 true  → 允许
    // 返回 false → 熔断器打开或 HALF_OPEN 探测槽已满（503）
    bool allow(const std::string& ip) {
        if (!_cfg.enabled || ip.empty()) return true;

        auto& s = _get_or_create(ip);
        switch (s.state) {
        case CbState::CLOSED:
            return true;

        case CbState::OPEN: {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - s.open_time).count();
            if (elapsed >= static_cast<long>(_cfg.timeout_seconds)) {
                // 超过等待时间 → 转入 HALF_OPEN，放行第一个探测请求
                s.state          = CbState::HALF_OPEN;
                s.success_count  = 0;
                s.half_open_reqs = 1;  // 已计入这个探测请求
                return true;
            }
            return false;  // 仍在 OPEN 冷却期
        }

        case CbState::HALF_OPEN:
            if (s.half_open_reqs < _cfg.half_open_max_reqs) {
                ++s.half_open_reqs;
                return true;
            }
            return false;  // 等待本批探测请求的结果
        }
        return true;
    }

    // 记录一次成功（dispatch 返回非 error 响应时调用）
    void record_success(const std::string& ip) {
        if (!_cfg.enabled || ip.empty()) return;
        auto& s = _get_or_create(ip);

        if (s.state == CbState::CLOSED) {
            s.failure_count = 0;  // 成功重置连续失败计数
        } else if (s.state == CbState::HALF_OPEN) {
            ++s.success_count;
            if (s.success_count >= _cfg.success_threshold) {
                s = {};  // 完全恢复，重置为 CLOSED
            }
        }
    }

    // 记录一次失败（dispatch 返回 JSON-RPC error 响应时调用）
    void record_failure(const std::string& ip) {
        if (!_cfg.enabled || ip.empty()) return;
        auto& s = _get_or_create(ip);

        if (s.state == CbState::CLOSED) {
            ++s.failure_count;
            if (s.failure_count >= _cfg.failure_threshold) {
                s.state     = CbState::OPEN;
                s.open_time = std::chrono::steady_clock::now();
            }
        } else if (s.state == CbState::HALF_OPEN) {
            // 探测请求失败 → 重新打开熔断，重置计时器
            s.state          = CbState::OPEN;
            s.open_time      = std::chrono::steady_clock::now();
            s.success_count  = 0;
            s.half_open_reqs = 0;
        }
    }

    // 查询某 IP 的当前熔断状态（供日志/监控使用）
    CbState get_state(const std::string& ip) const {
        auto it = _states.find(ip);
        return (it != _states.end()) ? it->second.state : CbState::CLOSED;
    }

    // 手动重置某 IP 的熔断状态（管理接口）
    void reset(const std::string& ip) {
        _states.erase(ip);
    }

private:
    CircuitState& _get_or_create(const std::string& ip) {
        return _states.emplace(ip, CircuitState{}).first->second;
    }
};

} // namespace mcp::security
