#pragma once
#include "mcp/security/security_policy.hh"
#include <arpa/inet.h>
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

namespace mcp::security {

// 编译后的 IPv4 CIDR 条目（网络地址 + 掩码，均为网络字节序）
struct CidrEntry {
    uint32_t network;
    uint32_t mask;
};

// IpFilter：无状态、只读，构造后线程安全
// 黑名单优先于白名单；白名单为空时由 default_allow 决定是否放行
class IpFilter {
    std::vector<CidrEntry> _whitelist;
    std::vector<CidrEntry> _blacklist;
    bool _default_allow;

public:
    IpFilter() : _default_allow(true) {}

    explicit IpFilter(const IpFilterConfig& cfg) : _default_allow(cfg.default_allow) {
        _blacklist.reserve(cfg.blacklist.size());
        for (const auto& s : cfg.blacklist) {
            _blacklist.push_back(_parse_cidr(s));
        }
        _whitelist.reserve(cfg.whitelist.size());
        for (const auto& s : cfg.whitelist) {
            _whitelist.push_back(_parse_cidr(s));
        }
    }

    // 检查 IPv4 点分十进制字符串是否允许通过
    // ip_str 为空（未知来源）时返回 default_allow
    bool is_allowed(const std::string& ip_str) const {
        if (ip_str.empty()) return _default_allow;

        uint32_t ip = _parse_ip(ip_str);
        if (ip == 0 && ip_str != "0.0.0.0") {
            // 解析失败（如 IPv6、域名等）→ 应用默认策略
            return _default_allow;
        }

        // 黑名单优先
        for (const auto& e : _blacklist) {
            if ((ip & e.mask) == e.network) return false;
        }

        // 白名单匹配
        if (!_whitelist.empty()) {
            for (const auto& e : _whitelist) {
                if ((ip & e.mask) == e.network) return true;
            }
            return false;  // 白名单非空但未命中 → 拒绝
        }

        return _default_allow;
    }

private:
    // 解析 IPv4 点分十进制，返回网络字节序，失败返回 0
    static uint32_t _parse_ip(const std::string& s) {
        struct in_addr addr{};
        if (::inet_pton(AF_INET, s.c_str(), &addr) == 1) {
            return addr.s_addr;
        }
        return 0;
    }

    // 解析 CIDR 字符串（如 "10.0.0.0/8" 或 "192.168.1.1"）
    static CidrEntry _parse_cidr(const std::string& cidr) {
        auto slash = cidr.find('/');
        std::string ip_part  = (slash == std::string::npos) ? cidr : cidr.substr(0, slash);
        int         prefix   = (slash == std::string::npos) ? 32
                             : std::stoi(cidr.substr(slash + 1));

        if (prefix < 0 || prefix > 32) {
            throw std::invalid_argument("Invalid CIDR prefix: " + cidr);
        }

        // 构造网络字节序掩码
        uint32_t mask_host = (prefix == 0) ? 0u : (~0u << (32 - prefix));
        uint32_t mask = htonl(mask_host);
        uint32_t net  = _parse_ip(ip_part) & mask;
        return {net, mask};
    }
};

} // namespace mcp::security
