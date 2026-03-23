# seastar-mcp-server

> 基于 C++20 + Seastar 的高性能 MCP（Model Context Protocol）SDK

---

## 目录

| 文档 | 说明 |
|---|---|
| 📖 [**完整技术文档**](complete-guide.md) | **推荐阅读**：架构、模块、接口、测试、性能一站式文档 |
| **架构与部署** | |
| [系统架构](architecture.md) | 模块划分、分片模型、数据流 |
| [构建与部署](build-deploy.md) | 依赖安装、编译、运行参数 |
| [Transport 层](transports.md) | HTTP/SSE、Streamable HTTP、StdIO |
| **API 参考** | |
| [McpServerBuilder](api/mcp-server-builder.md) | 服务器流式构建 API |
| [McpTool](api/mcp-tool.md) | Tool 接口与实现指南 |
| [McpResource](api/mcp-resource.md) | Resource 接口与实现指南 |
| [McpPrompt](api/mcp-prompt.md) | Prompt 接口与实现指南 |
| [高级 API](api/advanced.md) | 订阅通知、双向 RPC、进度推送 |
| **测试与性能** | |
| [测试指南](testing.md) | 单元测试、集成测试、运行方法 |
| [性能压测](benchmarking.md) | 单核基准、多核 QPS 扩展测试 |

---

## 项目简介

**seastar-mcp-server** 是一个 C++20 原生的 MCP 服务端 SDK，核心目标：

- **协议完整**：实现 MCP 2025-11-25 规范的全部方法（tools、resources、prompts、logging、sampling、elicitation 等）
- **高并发**：基于 Seastar 的 Share-Nothing 分片架构，每个 CPU 核心独立处理请求，无锁、无共享内存
- **低延迟**：全程 C++20 协程（`co_await`），无回调，无线程切换
- **多传输**：同时支持 HTTP/SSE（端口 8080）、Streamable HTTP（端口 8081）、StdIO 三种传输方式

---

## 快速上手

### 1. 最小 Server

```cpp
#include <mcp/mcp.hh>
#include <seastar/core/app-template.hh>
#include <csignal>

// 定义一个工具
class PingTool : public mcp::core::McpTool {
public:
    std::string get_name()  const override { return "ping"; }
    nlohmann::json get_definition() const override {
        return {
            {"name", "ping"},
            {"description", "返回 pong"},
            {"inputSchema", {{"type", "object"}, {"properties", nlohmann::json::object()}}}
        };
    }
    seastar::future<nlohmann::json> execute(const nlohmann::json&) override {
        nlohmann::json r;
        r["content"] = {{{"type", "text"}, {"text", "pong"}}};
        co_return r;
    }
};

int main(int argc, char** argv) {
    seastar::app_template app;
    return app.run(argc, argv, []() -> seastar::future<> {
        auto server = mcp::McpServerBuilder{}
            .name("my-server").version("1.0.0")
            .with_http(8080)
            .add_tool<PingTool>()
            .build();
        co_await server->start();

        seastar::promise<> stop;
        seastar::handle_signal(SIGINT, [&] { stop.set_value(); }, true);
        co_await stop.get_future();
        co_await server->stop();
    });
}
```

### 2. 编译

```bash
cd build && ninja
./examples/demo/demo_server -c4 -m512M --overprovisioned
```

### 3. 验证

```bash
# HTTP/SSE transport
curl http://127.0.0.1:8080/message \
  -d '{"jsonrpc":"2.0","id":1,"method":"ping","params":{}}' \
  -H "Content-Type: application/json"

# Streamable HTTP transport
curl http://127.0.0.1:8081/mcp \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}' \
  -H "Content-Type: application/json"
```

---

## 技术栈

| 组件 | 版本 | 用途 |
|---|---|---|
| C++ | 20 | 协程、ranges、concepts |
| [Seastar](https://seastar.io) | 23.x | 异步 I/O、HTTP 服务器、分片框架 |
| [nlohmann/json](https://github.com/nlohmann/json) | ≥ 3.11 | JSON 序列化/反序列化 |
| CMake | ≥ 3.15 | 构建系统 |
| Boost.Test | 任意 | C++ 单元测试框架 |
| pytest | ≥ 7 | Python 集成测试框架 |

---

## 协议支持矩阵

| MCP 方法 | 支持 | 说明 |
|---|---|---|
| `initialize` | ✅ | 返回 protocolVersion 2025-11-25 |
| `ping` | ✅ | 空结果 |
| `tools/list` | ✅ | 游标分页，page_size=50 |
| `tools/call` | ✅ | 自动补 `isError: false` |
| `resources/list` | ✅ | 游标分页 |
| `resources/read` | ✅ | 支持动态 URI 模板 |
| `resources/subscribe` | ✅ | 每核独立订阅表 |
| `resources/unsubscribe` | ✅ | |
| `resources/templates/list` | ✅ | |
| `prompts/list` | ✅ | 游标分页 |
| `prompts/get` | ✅ | |
| `logging/setLevel` | ✅ | 全局 Seastar 日志级别 |
| `roots/list` | ✅ | 返回空数组 |
| `completion/complete` | ✅ | Prompt 参数自动补全 |
| `notifications/initialized` | ✅ | 无操作 |
| `notifications/cancelled` | ✅ | 记录日志 |
| `sampling/createMessage` | ✅ | 框架级双向 RPC |
| `elicitation/create` | ✅ | 框架级双向 RPC |
