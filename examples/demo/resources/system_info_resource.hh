#pragma once
#include <mcp/core/interfaces.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/smp.hh>

namespace demo {

class SystemInfoResource : public mcp::core::McpResource {
public:
    std::string get_uri()  const override { return "sys://memory-info"; }
    std::string get_name() const override { return "seastar_memory_info"; }

    nlohmann::json get_definition() const override {
        return {
            {"uri", get_uri()},
            {"name", get_name()},
            {"mimeType", "application/json"},
            {"description", "提供 Seastar 引擎底层的实时内存分配和 CPU(SMP) 状态数据"}
        };
    }

    seastar::future<std::string> read() override {
        auto stats = seastar::memory::stats();
        nlohmann::json info = {
            {"total_memory_bytes",    stats.total_memory()},
            {"free_memory_bytes",     stats.free_memory()},
            {"allocated_memory_bytes",stats.allocated_memory()},
            {"large_allocations",     stats.large_allocations()},
            {"smp_core_count",        seastar::smp::count}
        };
        co_return info.dump(4);
    }
};

} // namespace demo
