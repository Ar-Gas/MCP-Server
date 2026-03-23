# McpServerBuilder API

`McpServerBuilder` 是构建 MCP Server 的入口，提供流式（Fluent）API。

**头文件**：`#include <mcp/mcp.hh>`（或 `#include <mcp/core/builder.hh>`）

---

## 接口速览

```cpp
class McpServerBuilder {
public:
    McpServerBuilder();

    // ── 服务器元数据 ─────────────────────────────────────────
    McpServerBuilder& name(std::string n);        // initialize 响应中的 serverInfo.name
    McpServerBuilder& version(std::string v);     // initialize 响应中的 serverInfo.version

    // ── 传输层开关 ───────────────────────────────────────────
    McpServerBuilder& with_http(uint16_t port = 8080);
    McpServerBuilder& with_streamable_http(uint16_t port = 8081);
    McpServerBuilder& with_stdio();

    // ── 组件注册 ─────────────────────────────────────────────
    template<typename T, typename... Args>
    McpServerBuilder& add_tool(Args&&... args);

    template<typename T, typename... Args>
    McpServerBuilder& add_resource(Args&&... args);

    template<typename T, typename... Args>
    McpServerBuilder& add_prompt(Args&&... args);

    // ── 构建 ─────────────────────────────────────────────────
    std::unique_ptr<server::McpServer> build();
};
```

---

## 详细说明

### `name(string)` / `version(string)`

设置服务器名称和版本，体现在 `initialize` 方法的响应中：

```json
{
  "result": {
    "protocolVersion": "2025-11-25",
    "serverInfo": { "name": "my-server", "version": "1.0.0" },
    "capabilities": { ... }
  }
}
```

### `with_http(port)`

启用 HTTP/SSE 传输（`HttpSseTransport`）。

- `GET /sse` — 建立 SSE 长连接，获取 session_id
- `POST /message?sessionId=...` — 发送 JSON-RPC 请求

默认端口：`8080`

### `with_streamable_http(port)`

启用 Streamable HTTP 传输（`StreamableHttpTransport`），符合 MCP 2024-11-05 规范。

- `POST /mcp` — 支持直接 JSON 响应和 SSE 流两种模式
- `GET /mcp` — 重连 SSE 流
- `DELETE /mcp` — 关闭 session

默认端口：`8081`

### `with_stdio()`

启用 StdIO 传输，从 stdin 读取 JSON-RPC 请求，向 stdout 写响应。
适用于 MCP 客户端以子进程方式启动服务器的场景（如 Claude Desktop）。

**注意**：StdIO 传输只在 shard 0 上运行，与多核 HTTP 传输完全独立。

### `add_tool<T>(args...)` / `add_resource<T>(args...)` / `add_prompt<T>(args...)`

注册组件，模板参数 `T` 为继承自 `McpTool` / `McpResource` / `McpPrompt` 的具体类，`args` 转发给其构造函数。

```cpp
// 无参构造
builder.add_tool<CalculateSumTool>();

// 带参构造
builder.add_tool<DatabaseQueryTool>("postgresql://...");
builder.add_resource<FileResource>("/var/data");
```

### `build()`

构建并返回 `unique_ptr<McpServer>`。调用后 Builder 不可复用。

---

## 完整示例

```cpp
#include <mcp/mcp.hh>
#include <seastar/core/app-template.hh>
#include <csignal>

int main(int argc, char** argv) {
    seastar::app_template app;
    return app.run(argc, argv, []() -> seastar::future<> {

        auto server = mcp::McpServerBuilder{}
            .name("production-server")
            .version("2.0.0")
            // 同时监听三种传输
            .with_http(8080)
            .with_streamable_http(8081)
            .with_stdio()
            // 注册工具
            .add_tool<CalculateSumTool>()
            .add_tool<GetCurrentTimeTool>()
            .add_tool<DatabaseQueryTool>("postgresql://localhost/mydb")
            // 注册资源
            .add_resource<SystemInfoResource>()
            .add_resource<LogFileResource>("/var/log/app.log")
            // 注册 Prompt
            .add_prompt<AnalyzeSystemPrompt>()
            .build();

        co_await server->start();

        // 等待退出信号
        seastar::promise<> stop_signal;
        seastar::handle_signal(SIGINT,  [&] { stop_signal.set_value(); }, true);
        seastar::handle_signal(SIGTERM, [&] { stop_signal.set_value(); }, true);
        co_await stop_signal.get_future();

        co_await server->stop();
    });
}
```

---

## Seastar app-template 启动参数

`app.run()` 的启动参数由 Seastar 解析，常用参数：

| 参数 | 说明 | 示例 |
|---|---|---|
| `-c N` | 使用 N 个 CPU 核心 | `-c 4` |
| `-m SIZE` | 分配内存大小 | `-m 512M` |
| `--overprovisioned` | 允许超额使用 CPU（共享机器必须开启） | |
| `--default-log-level=LEVEL` | Seastar 日志级别 | `--default-log-level=warn` |
| `--reactor-backend=BACKEND` | I/O 后端（epoll/io_uring） | `--reactor-backend=io_uring` |

```bash
# 生产环境：4核，限制512M，关闭多余日志
./my_server -c4 -m512M --overprovisioned --default-log-level=warn

# 开发调试：单核，打开全部日志
./my_server -c1 -m256M --overprovisioned --default-log-level=debug

# 性能压测：全核，大内存
./my_server -c$(nproc) -m2G --overprovisioned --reactor-backend=io_uring
```
