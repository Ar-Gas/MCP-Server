# Seastar MCP C++ SDK

基于 **C++20** 和 **Seastar 异步框架** 构建的高性能 **MCP (Model Context Protocol) C++ SDK**。

开发者只需引入 SDK、继承基类、通过 `McpServerBuilder` 注册能力，即可获得一个完整的、生产级 MCP 服务器——无需关心任何协议细节和网络底层。

---

## 核心特性

- **极致性能**：基于 Seastar 异步 I/O 框架，Share-Nothing 架构，真正多核并行。
- **现代 C++**：全面拥抱 C++20 协程（`co_await` / `co_return`），SDK 接口简洁，无回调地狱。
- **完整 MCP 协议支持**（`2025-11-25`）：
  - **Tools**：工具调用，`isError` 字段自动处理，支持 cursor 分页，支持 outputSchema 和 annotations。
  - **Resources**：静态资源 + URI 模板动态资源，支持 cursor 分页，支持订阅变更通知。
  - **Prompts**：带参数的提示词模板，支持 cursor 分页，支持参数自动补全。
  - **Logging**：`logging/setLevel` 动态调整 Seastar 全局日志级别，支持向客户端推送日志通知。
  - **Roots**：`roots/list` 返回服务端根列表。
  - **JSON-RPC Batch**：支持数组格式的批量请求，单次往返执行多条指令。
  - **双向 RPC**：支持服务端向客户端发起 `sampling/createMessage` 和 `elicitation/create` 请求。
- **三种 Transport，全部多核**：
  - **HTTP/SSE**（`:8080`）：GET `/sse` + POST `/message`，sessionId 会话管理，跨核 push 自动路由。
  - **Streamable HTTP**（`:8081`）：MCP 2025-11-25 单端点 `/mcp`，支持 JSON 直接响应与 SSE 流模式，`Mcp-Session-Id` header 会话管理。
  - **StdIO**：基于 `seastar::thread`，直接读 stdin / 写 stdout，兼容 Claude Desktop 等本地客户端。
- **真正多核**：`sharded<McpShard>` 架构，每核独立 dispatcher 和 session map，`http_server_control` 连接自动分配到各核，跨核 SSE push 通过 `invoke_on` 完成。
- **流式 Builder API**：`McpServerBuilder` 一链调用完成全部配置。

---

## 项目结构

```text
.
├── include/mcp/
│   ├── mcp.hh                                # SDK 单一公共入口（用户只需包含此文件）
│   ├── core/
│   │   ├── interfaces.hh                     # McpTool / McpResource / McpPrompt 抽象基类
│   │   ├── registry.hh                       # McpRegistry 统一注册表
│   │   └── builder.hh                        # McpServerBuilder 流式配置 API
│   ├── protocol/
│   │   └── json_rpc.hh                       # JSON-RPC 2.0 协议类型与错误码
│   ├── router/
│   │   └── dispatcher.hh                     # 异步 JSON-RPC 路由调度器（含 Batch 支持）
│   ├── transport/
│   │   ├── transport.hh                      # ITransport 抽象接口 + SseSession 定义
│   │   ├── stdio_transport.hh                # StdIO Transport（seastar::thread 实现）
│   │   ├── http_sse_transport.hh             # HTTP/SSE Transport（多核）
│   │   └── streamable_http_transport.hh      # Streamable HTTP Transport（MCP 2025-11-25）
│   └── server/
│       ├── mcp_shard.hh                      # McpShard：每核独立状态（dispatcher + sessions）
│       └── mcp_server.hh                     # McpServer：持有 sharded<McpShard>
├── src/mcp/server/
│   └── mcp_server.cc                         # MCP 方法注册与服务器实现
├── src/seastar_patches/
│   └── http_common_patch.cc                  # 修复 Seastar HTTP SSE flush bug
├── examples/demo/                            # 完整示例应用
│   ├── main.cc                               # 使用 SDK 的示例入口
│   ├── tools/                                # 示例 Tool 实现
│   ├── resources/                            # 示例 Resource 实现
│   └── prompts/                              # 示例 Prompt 实现
├── tests/
│   ├── unit/                                 # C++ 单元测试（Boost.Test + Seastar Testing）
│   ├── integration/                          # Python 集成测试（pytest，21 个用例）
│   └── perf/                                 # 性能压测脚本
├── docs/                                     # 详细技术文档
├── Dockerfile
└── CMakeLists.txt                            # 导出 mcp_sdk::mcp_sdk target
```

---

## 编译与构建

**系统要求**：Linux（Ubuntu 24.04 推荐）或 WSL2，GCC 13+ / Clang 16+，CMake 3.15+，Ninja，已安装 Seastar 和 nlohmann-json3-dev。

```bash
mkdir build && cd build
cmake .. -G Ninja
ninja
```

编译产物：
- `build/examples/demo/demo_server` — 示例可执行文件
- `build/tests/test_dispatcher`、`build/tests/test_registry` — 单元测试

---

## 快速上手

### 1. 引入 SDK

```cmake
add_subdirectory(seastar-mcp-sdk)
target_link_libraries(my_server PRIVATE mcp_sdk::mcp_sdk)
```

### 2. 实现一个 Tool

```cpp
#include <mcp/mcp.hh>

class MyTool : public mcp::core::McpTool {
public:
    std::string get_name() const override { return "my_tool"; }

    nlohmann::json get_definition() const override {
        return {
            {"name", get_name()},
            {"description", "我的工具"},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"input", {{"type", "string"}, {"description", "输入文本"}}}
                }},
                {"required", nlohmann::json::array({"input"})}
            }}
        };
    }

    seastar::future<nlohmann::json> execute(const nlohmann::json& args) override {
        std::string input = args.value("input", "");
        nlohmann::json result;
        result["content"] = nlohmann::json::array({{{"type", "text"}, {"text", "结果: " + input}}});
        co_return result;
    }
};
```

### 3. 启动服务器

```cpp
#include <mcp/mcp.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/signal.hh>

int main(int argc, char** argv) {
    seastar::app_template app;
    return app.run(argc, argv, []() -> seastar::future<> {
        auto server = mcp::McpServerBuilder{}
            .name("my-mcp-server")
            .version("1.0.0")
            .with_http(8080)             // HTTP/SSE，GET /sse + POST /message
            .with_streamable_http(8081)  // Streamable HTTP，POST /mcp
            .with_stdio()                // StdIO，兼容 Claude Desktop 等客户端
            .add_tool<MyTool>()
            .build();

        co_await server->start();

        seastar::promise<> stop;
        seastar::handle_signal(SIGINT,  [&] { stop.set_value(); }, true);
        seastar::handle_signal(SIGTERM, [&] { stop.set_value(); }, true);
        co_await stop.get_future();

        co_await server->stop();
    });
}
```

---

## 运行与测试

### 方法一：MCP Inspector（推荐调试）

```bash
npx @modelcontextprotocol/inspector ./build/examples/demo/demo_server -- -c 1 --default-log-level=warn
```

浏览器将自动打开图形化界面，可直接测试所有 Tools、Resources 和 Prompts，并实时查看 JSON-RPC 交互日志。

### 方法二：HTTP/SSE 模式（`:8080`）

```bash
./build/examples/demo/demo_server -c 4 -m 512M --default-log-level=warn
```

<details>
<summary>展开查看 curl 测试命令</summary>

```bash
# 初始化握手
curl -s -X POST http://127.0.0.1:8080/message \
     -H 'Content-Type: application/json' \
     -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}}'

# 获取工具列表（支持 cursor 分页）
curl -s -X POST http://127.0.0.1:8080/message \
     -H 'Content-Type: application/json' \
     -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'

# 调用工具（响应自动包含 isError: false）
curl -s -X POST http://127.0.0.1:8080/message \
     -H 'Content-Type: application/json' \
     -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"calculate_sum","arguments":{"a":3,"b":4}}}'

# 读取资源
curl -s -X POST http://127.0.0.1:8080/message \
     -H 'Content-Type: application/json' \
     -d '{"jsonrpc":"2.0","id":4,"method":"resources/read","params":{"uri":"sys://memory-info"}}'

# 订阅资源变更（需先建立 SSE session）
curl -s -X POST 'http://127.0.0.1:8080/message?sessionId=s0_1' \
     -H 'Content-Type: application/json' \
     -d '{"jsonrpc":"2.0","id":5,"method":"resources/subscribe","params":{"uri":"sys://memory-info"}}'

# 获取提示词
curl -s -X POST http://127.0.0.1:8080/message \
     -H 'Content-Type: application/json' \
     -d '{"jsonrpc":"2.0","id":6,"method":"prompts/get","params":{"name":"analyze_server_health","arguments":{"focus":"memory"}}}'

# JSON-RPC Batch（单次往返执行多条指令）
curl -s -X POST http://127.0.0.1:8080/message \
     -H 'Content-Type: application/json' \
     -d '[{"jsonrpc":"2.0","id":1,"method":"ping"},{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}]'

# 动态调整日志级别
curl -s -X POST http://127.0.0.1:8080/message \
     -H 'Content-Type: application/json' \
     -d '{"jsonrpc":"2.0","id":7,"method":"logging/setLevel","params":{"level":"debug"}}'

# SSE 长连接（另开终端）
curl -N http://127.0.0.1:8080/sse
```

</details>

### 方法三：Streamable HTTP 模式（`:8081`，MCP 2025-11-25）

<details>
<summary>展开查看 curl 测试命令</summary>

```bash
# 简单请求/响应（无 session）
curl -s -X POST http://127.0.0.1:8081/mcp \
     -H 'Content-Type: application/json' \
     -d '{"jsonrpc":"2.0","id":1,"method":"ping","params":{}}'

# 建立 SSE 流 session（Accept: text/event-stream）
# 服务端在响应 header 中返回 Mcp-Session-Id
curl -v -X POST http://127.0.0.1:8081/mcp \
     -H 'Content-Type: application/json' \
     -H 'Accept: text/event-stream' \
     -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}}'

# 向已有 session 发送请求（结果通过 SSE 流推送）
curl -s -X POST http://127.0.0.1:8081/mcp \
     -H 'Content-Type: application/json' \
     -H 'Mcp-Session-Id: <上一步返回的 session id>' \
     -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'

# 关闭 session
curl -s -X DELETE http://127.0.0.1:8081/mcp \
     -H 'Mcp-Session-Id: <session id>'
```

</details>

### 方法四：StdIO 模式（管道测试）

```bash
echo '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}' \
  | ./build/examples/demo/demo_server -c 1 --default-log-level=warn
```

---

## 接入 AI 客户端

### Claude Desktop / Cursor（StdIO 模式）

编辑配置文件（`~/.claude/claude_desktop_config.json` 或 `%APPDATA%\Claude\claude_desktop_config.json`）：

```json
{
  "mcpServers": {
    "seastar_cpp": {
      "command": "/绝对路径/build/examples/demo/demo_server",
      "args": ["-c", "1", "-m", "512M", "--default-log-level=warn"]
    }
  }
}
```

### Docker 部署

```bash
docker build -t seastar-mcp-sdk:latest .
```

```json
{
  "mcpServers": {
    "seastar_docker": {
      "command": "docker",
      "args": ["run", "-i", "--rm", "seastar-mcp-sdk:latest"]
    }
  }
}
```

> `docker run` 必须包含 `-i` 以保持 stdin 开启。

---

## 扩展 SDK

添加新能力只需三步，无需修改任何底层代码：

**1. 继承基类，实现业务逻辑**

在自己的项目中新建头文件，继承 `mcp::core::McpTool`、`mcp::core::McpResource` 或 `mcp::core::McpPrompt`。

**2. 通过 Builder 注册**

```cpp
mcp::McpServerBuilder{}
    .with_http(8080)
    .with_streamable_http(8081)
    .add_tool<MyNewTool>()
    .add_resource<MyNewResource>()
    .add_prompt<MyNewPrompt>()
    .build();
```

**3. 编译运行**

无需接触路由、协议、传输层的任何代码。

---

## 已支持的 MCP 方法（协议版本 2025-11-25）

| 方法 | 说明 |
|------|------|
| `initialize` | 协议握手，返回 `protocolVersion: "2025-11-25"` 及服务端能力声明 |
| `ping` | 心跳检测 |
| `tools/list` | 列出所有工具（支持 cursor 分页，含 title / outputSchema / annotations） |
| `tools/call` | 调用指定工具，自动补充 `isError: false` |
| `resources/list` | 列出所有资源（支持 cursor 分页） |
| `resources/templates/list` | 列出 URI 模板资源 |
| `resources/read` | 读取资源内容，支持动态 URI 模板 |
| `resources/subscribe` | 订阅资源变更，断连自动清理 |
| `resources/unsubscribe` | 取消资源订阅 |
| `prompts/list` | 列出所有提示词模板（支持 cursor 分页） |
| `prompts/get` | 获取并渲染提示词 |
| `completion/complete` | 参数自动补全 |
| `logging/setLevel` | 动态调整服务端日志级别 |
| `roots/list` | 列出服务端根路径 |
| `notifications/initialized` | 客户端初始化完成通知（no-op） |
| `notifications/cancelled` | 请求取消通知（记录日志） |
| `sampling/createMessage` | 服务端向客户端发起 LLM 推理请求（双向 RPC） |
| `elicitation/create` | 服务端向客户端请求用户输入（双向 RPC） |

---

## 文档

详细技术文档见 [`docs/`](docs/) 目录：

| 文档 | 说明 |
|------|------|
| [完整技术文档](docs/complete-guide.md) | 架构、模块、接口、测试、性能一站式文档（推荐） |
| [系统架构](docs/architecture.md) | 分片模型、数据流、关键设计决策 |
| [SDK API 参考](docs/api/) | McpTool / McpResource / McpPrompt / 高级接口 |
| [Transport 层](docs/transports.md) | 三种传输方式详解 |
| [测试指南](docs/testing.md) | 单元测试、集成测试、手动验证 |
| [性能压测](docs/benchmarking.md) | 基准测试方法与结果分析 |
| [构建与部署](docs/build-deploy.md) | 依赖安装、编译参数、生产调优 |
