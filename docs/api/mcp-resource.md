# McpResource 接口

Resource 是 MCP 中可被 AI 客户端读取的"数据源"，可以是静态文件、实时数据、数据库查询结果等。

**头文件**：`#include <mcp/core/interfaces.hh>`

---

## 基类定义

```cpp
namespace mcp::core {

class McpResource {
public:
    virtual ~McpResource() = default;

    // ── 必须实现 ────────────────────────────────────────────────────────────
    virtual std::string    get_uri()        const = 0;   // 资源唯一 URI
    virtual std::string    get_name()       const = 0;   // 资源名称（标识符）
    virtual nlohmann::json get_definition() const = 0;   // 资源定义
    virtual seastar::future<std::string> read() = 0;     // 读取内容（协程）

    // ── 可选覆盖 ─────────────────────────────────────────────────────────
    virtual std::string    get_title()      const { return get_name(); }
    virtual std::optional<std::string>
        get_icon_uri()                      const { return std::nullopt; }
};

} // namespace mcp::core
```

---

## 方法详解

### `get_uri()` — 资源 URI

全局唯一的资源标识符，遵循 URI 格式。客户端在 `resources/read` 中通过 `uri` 字段指定。

**URI 命名建议**：
- 静态资源：`scheme://path`，如 `sys://memory-info`、`file:///etc/hosts`
- 动态模板：在 `resources/templates/list` 中声明，如 `sys://metrics/{component}`

```cpp
std::string get_uri() const override { return "sys://memory-info"; }
```

### `get_name()` — 资源名称

机器可读的唯一标识，用于列表展示和日志。

```cpp
std::string get_name() const override { return "seastar_memory_info"; }
```

### `get_definition()` — 资源定义

返回完整的资源描述 JSON，必须包含 `uri`、`name`、`mimeType`、`description`。

```cpp
nlohmann::json get_definition() const override {
    return {
        {"uri",         get_uri()},
        {"name",        get_name()},
        {"mimeType",    "application/json"},
        {"description", "Seastar 引擎实时内存与 CPU 状态"}
    };
}
```

**常用 mimeType**：

| 类型 | mimeType |
|---|---|
| JSON 数据 | `application/json` |
| 纯文本 | `text/plain` |
| Markdown | `text/markdown` |
| HTML | `text/html` |
| 二进制 | `application/octet-stream` |

### `read()` — 读取内容

协程，返回资源内容字符串（由框架包装进 `contents[].text`）。

`resources/read` 的响应格式由框架自动构造：
```json
{
  "contents": [
    {
      "uri": "sys://memory-info",
      "mimeType": "application/json",
      "text": "<read() 的返回值>"
    }
  ]
}
```

---

## 完整示例

### 示例 1：系统信息资源（JSON）

```cpp
#include <mcp/core/interfaces.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/smp.hh>

class SystemInfoResource : public mcp::core::McpResource {
public:
    std::string get_uri()  const override { return "sys://memory-info"; }
    std::string get_name() const override { return "seastar_memory_info"; }

    nlohmann::json get_definition() const override {
        return {
            {"uri",         get_uri()},
            {"name",        get_name()},
            {"mimeType",    "application/json"},
            {"description", "Seastar 引擎底层实时内存分配和 CPU(SMP) 状态数据"}
        };
    }

    seastar::future<std::string> read() override {
        auto stats = seastar::memory::stats();
        nlohmann::json info = {
            {"total_memory_bytes",     stats.total_memory()},
            {"free_memory_bytes",      stats.free_memory()},
            {"allocated_memory_bytes", stats.allocated_memory()},
            {"large_allocations",      stats.large_allocations()},
            {"smp_core_count",         seastar::smp::count}
        };
        co_return info.dump(4);
    }
};
```

### 示例 2：文件资源（异步读取）

```cpp
#include <mcp/core/interfaces.hh>
#include <seastar/core/file.hh>
#include <seastar/core/fstream.hh>

class LogFileResource : public mcp::core::McpResource {
    std::string _path;
public:
    explicit LogFileResource(std::string path) : _path(std::move(path)) {}

    std::string get_uri()  const override { return "file://" + _path; }
    std::string get_name() const override { return "log_file"; }

    nlohmann::json get_definition() const override {
        return {
            {"uri",         get_uri()},
            {"name",        get_name()},
            {"mimeType",    "text/plain"},
            {"description", "应用日志文件（最后 100 行）"}
        };
    }

    seastar::future<std::string> read() override {
        auto f = co_await seastar::open_file_dma(_path, seastar::open_flags::ro);
        auto size = co_await f.size();
        // 读最后 64KB
        auto read_size = std::min(size, uint64_t{65536});
        auto offset    = size - read_size;
        auto buf = co_await f.dma_read<char>(offset, read_size);
        co_await f.close();
        co_return std::string(buf.get(), buf.size());
    }
};
```

### 示例 3：配置资源（内存数据，不需要异步）

```cpp
#include <mcp/core/interfaces.hh>

class ServerConfigResource : public mcp::core::McpResource {
public:
    std::string get_uri()  const override { return "config://server"; }
    std::string get_name() const override { return "server_config"; }

    nlohmann::json get_definition() const override {
        return {
            {"uri", get_uri()}, {"name", get_name()},
            {"mimeType", "application/json"},
            {"description", "当前服务器运行时配置"}
        };
    }

    seastar::future<std::string> read() override {
        nlohmann::json cfg = {
            {"version",    "2.0.0"},
            {"cores",      seastar::smp::count},
            {"build_type", "release"}
        };
        co_return cfg.dump(2);
    }
};
```

---

## 订阅与变更通知

资源支持订阅机制：客户端订阅后，服务器数据变化时可主动推送 `notifications/resources/updated`。

### 服务端触发更新通知

```cpp
// 在 McpServer 方法中触发
co_await server->broadcast_resource_updated("sys://memory-info");
```

`broadcast_resource_updated()` 会向所有核心上订阅了该 URI 的 SSE session 推送：
```json
{
  "jsonrpc": "2.0",
  "method": "notifications/resources/updated",
  "params": { "uri": "sys://memory-info" }
}
```

详见 [高级 API](advanced.md)。

---

## `resources/read` 请求/响应格式

**请求**：
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "resources/read",
  "params": { "uri": "sys://memory-info" }
}
```

**响应**：
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "contents": [
      {
        "uri":      "sys://memory-info",
        "mimeType": "application/json",
        "text":     "{\n  \"total_memory_bytes\": 536870912, ...\n}"
      }
    ]
  }
}
```

---

## 注意事项

1. **URI 全局唯一**：同一个 `McpServer` 中不能注册两个 URI 相同的资源。
2. **`read()` 必须是协程**：即使直接 `co_return` 字符串字面量也可以。
3. **mimeType 与内容一致**：如果 `mimeType` 是 `application/json`，`read()` 应返回合法 JSON 字符串；否则客户端解析可能失败。
4. **动态 URI（模板资源）**：对于 `sys://metrics/{component}` 这类 URI 模板，在 `mcp_server.cc` 的 `resources/read` handler 中通过前缀匹配路由（`sys://metrics/` → 自定义读取逻辑），不依赖 `McpRegistry`。
