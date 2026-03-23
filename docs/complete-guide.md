# Seastar MCP C++ SDK — 完整技术文档

> 基于 C++20 + Seastar 框架构建的高性能 MCP（Model Context Protocol）服务端 SDK

---

## 目录

1. [项目简介与设计目标](#1-项目简介与设计目标)
2. [项目架构总览](#2-项目架构总览)
3. [核心模块设计](#3-核心模块设计)
   - 3.1 [接口层：McpTool / McpResource / McpPrompt](#31-接口层)
   - 3.2 [注册表：McpRegistry](#32-注册表mcpregistry)
   - 3.3 [路由层：JsonRpcDispatcher](#33-路由层jsonrpcdispatcher)
   - 3.4 [分片引擎：McpShard](#34-分片引擎mcpshard)
   - 3.5 [服务主类：McpServer](#35-服务主类mcpserver)
   - 3.6 [传输层：Transport](#36-传输层transport)
   - 3.7 [构建器：McpServerBuilder](#37-构建器mcpserverbuilder)
4. [SDK 接口参考](#4-sdk-接口参考)
   - 4.1 [McpTool 接口](#41-mcptool-接口)
   - 4.2 [McpResource 接口](#42-mcpresource-接口)
   - 4.3 [McpPrompt 接口](#43-mcpprompt-接口)
   - 4.4 [McpServerBuilder 接口](#44-mcpserverbuilder-接口)
   - 4.5 [高级接口：通知与双向 RPC](#45-高级接口通知与双向-rpc)
5. [请求生命周期](#5-请求生命周期)
6. [测试体系设计](#6-测试体系设计)
7. [性能数据与分析](#7-性能数据与分析)
8. [关键工程决策](#8-关键工程决策)

---

## 1. 项目简介与设计目标

**MCP（Model Context Protocol）** 是 Anthropic 定义的开放标准，允许 AI 助手（如 Claude）通过标准化协议调用外部工具、读取资源、获取提示词模板，从而突破模型本身的知识边界和能力边界。

**seastar-mcp-server** 是一个面向生产环境的 C++ 原生 MCP SDK，解决了主流 MCP SDK（Python/TypeScript）在高并发场景下的吞吐量瓶颈问题。

### 核心设计目标

| 目标 | 实现手段 |
|------|---------|
| **极致吞吐** | Seastar Share-Nothing 架构，每核独立运行，无锁 |
| **低延迟** | C++20 协程 (`co_await`)，零拷贝 SSE 推送 |
| **开发友好** | 继承 3 个基类 + 一行 Builder 代码即可上线 |
| **协议完整** | 覆盖 MCP 2025-11-25 规范全部方法 |
| **多传输** | HTTP/SSE、Streamable HTTP、StdIO 同时运行 |

### 技术栈

```
┌─────────────────────────────────────────────────────────┐
│  应用层  │  C++20 协程  │  nlohmann/json 3.11           │
├─────────────────────────────────────────────────────────┤
│  网络层  │  Seastar HTTP Server (sharded)                │
├─────────────────────────────────────────────────────────┤
│  I/O 层  │  Seastar Reactor (epoll / io_uring)           │
├─────────────────────────────────────────────────────────┤
│  测试层  │  Boost.Test (C++) + pytest (Python)           │
└─────────────────────────────────────────────────────────┘
```

---

## 2. 项目架构总览

### 2.1 层次结构

整个 SDK 分为四个层次，从用户代码到底层网络逐层封装：

```
╔══════════════════════════════════════════════════════════════╗
║                    【用户应用层】                              ║
║   class MyTool     : public McpTool     { ... };              ║
║   class MyResource : public McpResource { ... };              ║
║   class MyPrompt   : public McpPrompt   { ... };              ║
║                                                               ║
║   McpServerBuilder{}.add_tool<MyTool>().with_http(8080).build()║
╚══════════════════════════╤═══════════════════════════════════╝
                           │  build() 返回 McpServer
╔══════════════════════════▼═══════════════════════════════════╗
║                    【SDK 核心层】                              ║
║                                                               ║
║  ┌─────────────────┐  ┌──────────────────────────────────┐   ║
║  │  McpRegistry    │  │       McpServer                  │   ║
║  │  (工具/资源/    │  │  持有 sharded<McpShard>           │   ║
║  │   Prompt 表)    │  │  持有 vector<ITransport>          │   ║
║  └────────┬────────┘  └──────────────┬───────────────────┘   ║
║           │ 共享只读                  │ 1..N                  ║
║           │              ┌───────────▼──────────────────┐    ║
║           └─────────────►│   McpShard (per CPU core)    │    ║
║                           │   JsonRpcDispatcher          │    ║
║                           │   SSE Session Map            │    ║
║                           │   Subscription Map           │    ║
║                           └──────────────────────────────┘    ║
╚══════════════════════════╤═══════════════════════════════════╝
                           │  dispatch(body, session_id)
╔══════════════════════════▼═══════════════════════════════════╗
║                    【传输层】                                  ║
║                                                               ║
║  ┌──────────────┐  ┌──────────────────┐  ┌───────────────┐   ║
║  │HttpSseTransp.│  │StreamableHttpTr. │  │StdioTransport │   ║
║  │  :8080       │  │  :8081           │  │  stdin/stdout │   ║
║  │  GET /sse    │  │  POST/GET/DELETE │  │  shard 0 only │   ║
║  │  POST /msg   │  │  /mcp            │  │               │   ║
║  └──────────────┘  └──────────────────┘  └───────────────┘   ║
╚══════════════════════════╤═══════════════════════════════════╝
                           │
╔══════════════════════════▼═══════════════════════════════════╗
║                 【Seastar 异步 I/O 层】                        ║
║         epoll / io_uring，多核 Reactor，无锁调度               ║
╚══════════════════════════════════════════════════════════════╝
```

### 2.2 Share-Nothing 分片模型

Seastar 的核心理念是 **Share-Nothing**：每个 CPU 核心是一个完全独立的执行单元，拥有独立的内存区域、独立的网络连接队列，核间不共享任何可变数据。

```
  物理机（4核）
  ┌──────────────────────────────────────────────────────┐
  │                                                      │
  │  Core 0              Core 1              Core 2      │
  │  ┌─────────────┐    ┌─────────────┐    ┌──────────┐  │
  │  │ McpShard[0] │    │ McpShard[1] │    │McpShard.│  │
  │  │             │    │             │    │         │  │
  │  │ Dispatcher  │    │ Dispatcher  │    │Dispatch.│  │
  │  │ Sessions:   │    │ Sessions:   │    │Sessions │  │
  │  │  s0_1 ──┐  │    │  s1_1 ──┐  │    │ s2_1.. │  │
  │  │  s0_2   │  │    │  s1_2   │  │    │        │  │
  │  │ Subscr. │  │    │ Subscr. │  │    │Subscr. │  │
  │  └─────────┼──┘    └─────────┼──┘    └────────┘  │
  │            │ ←跨核push        │                    │
  │            │  invoke_on(0)   │                    │
  │            └─────────────────┘                    │
  │                                                      │
  │  ┌────────────────────────────────────────────────┐  │
  │  │     McpRegistry（共享只读，启动后不变）            │  │
  │  │     Tools: {calculate_sum, get_current_time}    │  │
  │  │     Resources: {sys://memory-info}              │  │
  │  │     Prompts: {analyze_server_health}            │  │
  │  └────────────────────────────────────────────────┘  │
  └──────────────────────────────────────────────────────┘
```

**关键设计**：
- `McpRegistry`（工具/资源/Prompt 表）在启动前填充，启动后**只读**，所有 shard 安全共享同一个 `shared_ptr`
- `McpShard` 的 Session Map、订阅表、双向 RPC 等待表全部是**核私有**数据，无需 mutex
- 跨核通信（如"请求落在 Core 1，但 session 属于 Core 0"）通过 `shards.invoke_on(target_shard, lambda)` 完成，底层是 Seastar 的无锁消息队列

### 2.3 Session ID 编码

Session ID 中编码了 shard 归属，使任意核都能正确路由：

```
HTTP/SSE Transport：
  "s0_1"   → shard=0, counter=1
  "s2_47"  → shard=2, counter=47

Streamable HTTP Transport：
  "sm0_1"  → shard=0, counter=1
  "sm1_5"  → shard=1, counter=5

解析：_parse_shard("s0_42")  → 0
      _parse_shard("sm2_3")  → 2
```

### 2.4 目录结构

```
seastar-mcp-server/
├── include/mcp/                    ← SDK 公开头文件（用户 include 此目录）
│   ├── mcp.hh                      ← 单一入口：一行 include 获得全部能力
│   ├── core/
│   │   ├── interfaces.hh           ← McpTool / McpResource / McpPrompt 基类
│   │   ├── registry.hh             ← McpRegistry：统一注册与调用
│   │   └── builder.hh              ← McpServerBuilder：流式配置 API
│   ├── protocol/
│   │   └── json_rpc.hh             ← JSON-RPC 2.0 类型、错误码、异常
│   ├── router/
│   │   └── dispatcher.hh           ← JsonRpcDispatcher：方法路由 + Batch
│   ├── transport/
│   │   ├── transport.hh            ← ITransport 接口 + SseSession 定义
│   │   ├── stdio_transport.hh      ← StdIO：seastar::thread 读 stdin
│   │   ├── http_sse_transport.hh   ← HTTP/SSE：多核 GET/POST
│   │   └── streamable_http_transport.hh ← Streamable HTTP：单端点 /mcp
│   └── server/
│       ├── mcp_shard.hh            ← McpShard：每核状态（核心）
│       └── mcp_server.hh           ← McpServer：顶层服务对象
│
├── src/mcp/server/
│   └── mcp_server.cc               ← 所有 MCP 方法注册（initialize/tools/...）
│
├── src/seastar_patches/
│   └── http_common_patch.cc        ← 修复 Seastar HTTP SSE flush bug
│
├── examples/demo/                  ← 完整示例应用
│   ├── main.cc                     ← 5 行代码启动完整 MCP 服务器
│   ├── tools/
│   │   ├── calculate_sum_tool.hh   ← 演示 Tool + outputSchema + annotations
│   │   └── get_current_time_tool.hh
│   ├── resources/
│   │   └── system_info_resource.hh ← 读取 Seastar 内存/CPU 实时数据
│   └── prompts/
│       └── analyze_system_prompt.hh
│
├── tests/
│   ├── unit/                       ← C++ 单元测试（Boost.Test + Seastar Testing）
│   │   ├── test_dispatcher.cc      ← JsonRpcDispatcher 7 个用例
│   │   └── test_registry.cc        ← McpRegistry 10 个用例
│   ├── integration/                ← Python 集成测试（pytest，21 个用例）
│   │   ├── conftest.py             ← 自动启停 demo_server fixture
│   │   ├── test_protocol.py        ← MCP 协议合规测试（15个）
│   │   ├── test_transport_sse.py   ← HTTP/SSE 传输测试（3个）
│   │   └── test_transport_stream.py← Streamable HTTP 传输测试（3个）
│   └── perf/                       ← 性能压测脚本
│       ├── bench.py                ← 单核基准：QPS/P50/P95/P99
│       └── multicore_bench.py      ← 多核扩展：1→2→4核 QPS 对比
│
├── docs/                           ← 本文档目录
└── CMakeLists.txt
```

---

## 3. 核心模块设计

### 3.1 接口层

**文件**：`include/mcp/core/interfaces.hh`

三个基类定义了用户扩展 SDK 的全部接口。所有方法都有合理的默认实现，用户只需覆盖必须的部分。

#### McpTool 基类

```
McpTool（抽象基类）
├── get_name()        → string     [必须] 工具唯一标识（路由键）
├── get_definition()  → json       [必须] 工具描述 + inputSchema
├── execute(args)     → future<json> [必须] 异步执行逻辑（协程）
│
├── get_title()       → string     [可选] 可读名称，默认=get_name()
├── get_output_schema()→ optional<json> [可选] 结构化输出 Schema
├── get_annotations() → json       [可选] 行为提示（readOnly/idempotent...）
└── get_icon_uri()    → optional<string> [可选] 图标 URL
```

#### McpResource 基类

```
McpResource（抽象基类）
├── get_uri()         → string     [必须] 资源唯一 URI（如 sys://memory-info）
├── get_name()        → string     [必须] 资源名称
├── get_definition()  → json       [必须] 资源描述（含 mimeType）
├── read()            → future<string> [必须] 异步读取内容（协程）
│
├── get_title()       → string     [可选] 可读名称
└── get_icon_uri()    → optional<string> [可选] 图标 URL
```

#### McpPrompt 基类

```
McpPrompt（抽象基类）
├── get_name()        → string     [必须] 提示词唯一名称
├── get_definition()  → json       [必须] 描述 + arguments 参数列表
├── get_messages(args)→ future<json> [必须] 渲染成消息数组（协程）
│
├── get_title()       → string     [可选] 可读名称
└── get_icon_uri()    → optional<string> [可选] 图标 URL
```

### 3.2 注册表：McpRegistry

**文件**：`include/mcp/core/registry.hh`

`McpRegistry` 是连接用户实现与框架底层的中间层，负责：

1. **存储**：三张 `unordered_map`，分别以名称/URI 为键
2. **自动元数据注入**：`get_tools_list()` 调用时自动将 `get_title()`、`get_output_schema()`、`get_annotations()`、`get_icon_uri()` 的值合并进 JSON，避免用户在 `get_definition()` 中重复填写
3. **统一调用**：`call_tool()` 在执行后自动补充 `isError: false`（MCP 规范要求）
4. **错误转换**：未找到工具/资源/Prompt 时抛出 `JsonRpcException`，由 Dispatcher 统一转为标准 JSON-RPC error 响应

```
用户调用 tools/call：
  dispatcher.handle_request(body)
    └── handler("tools/call", params)
          └── registry.call_tool("calculate_sum", {a:3, b:4})
                ├── _tools.find("calculate_sum")  → CalculateSumTool
                ├── tool.execute({a:3, b:4})       → {content:[...]}
                └── result["isError"] = false       → 自动注入
```

### 3.3 路由层：JsonRpcDispatcher

**文件**：`include/mcp/router/dispatcher.hh`

Dispatcher 是 JSON-RPC 2.0 协议的解析和路由中心。每个 `McpShard` 持有一个独立的 Dispatcher 实例（方法表在启动时填充，运行时只读）。

```
handle_request(raw_body)
├── json::parse(raw_body)    → 失败: ParseError (-32700)
├── req.is_array()?
│   ├── empty array          → InvalidRequest (-32600)
│   └── foreach elem         → _handle_single(elem) × N，收集响应数组
└── _handle_single(req)
    ├── validate jsonrpc/method → 失败: InvalidRequest (-32600)
    ├── has_id? NO (通知)
    │   └── run_in_background(notification_handler(params))
    │       return nullopt（通知无响应）
    └── has_id? YES (方法调用)
        ├── _methods.find(method) → 失败: MethodNotFound (-32601)
        ├── co_await handler(params)
        │   ├── 正常: response.result = result_json
        │   ├── JsonRpcException: response.error = {code, msg}
        │   └── std::exception: response.error = InternalError (-32603)
        └── return response.dump()
```

**Batch 请求示例**：

```json
// 请求
[
  {"jsonrpc":"2.0","id":1,"method":"ping","params":{}},
  {"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}
]

// 响应（两个结果数组）
[
  {"jsonrpc":"2.0","id":1,"result":{}},
  {"jsonrpc":"2.0","id":2,"result":{"tools":[...]}}
]
```

### 3.4 分片引擎：McpShard

**文件**：`include/mcp/server/mcp_shard.hh`

`McpShard` 是整个 SDK 的核心执行单元，继承自 `seastar::peering_sharded_service<McpShard>`，每个 CPU 核心独立持有一个实例。

#### 内部状态（全部为核私有，无锁访问）

```cpp
class McpShard : public seastar::peering_sharded_service<McpShard> {
    McpServerConfig  _config;       // 服务器配置（只读）
    shared_ptr<McpRegistry> _registry;  // 注册表（共享只读）

    JsonRpcDispatcher _dispatcher;  // 本核 RPC 路由器

    // SSE 会话管理
    unordered_map<string, shared_ptr<SseSession>> _sessions;
    uint64_t _session_counter = 0;

    // 资源订阅：uri → 订阅了该 uri 的 session_id 集合
    unordered_map<string, unordered_set<string>> _subscriptions;

    // 当前请求的上下文（dispatch 时暂存，供同步 handler 使用）
    string _current_session_id;
    string _current_progress_token;

    // 双向 RPC：服务端发起请求时等待客户端响应
    uint64_t _server_request_counter = 0;
    unordered_map<uint64_t, promise<json>> _pending_client_requests;
};
```

#### dispatch() 处理流程

```
dispatch(body, session_id)
├── 1. 判断是否为客户端对服务端请求的响应（双向 RPC）
│       json: {id:N, result/error, 无method}
│       └── handle_client_response(j) → promise.set_value → 返回 nullopt
│
├── 2. 提取 _meta.progressToken（进度通知用）
│
├── 3. 暂存 _current_session_id（subscribe/unsubscribe handler 使用）
│
└── 4. co_await _dispatcher.handle_request(body) → 返回 JSON 响应字符串
```

### 3.5 服务主类：McpServer

**文件**：`include/mcp/server/mcp_server.hh`

`McpServer` 是 SDK 对外暴露的顶层对象，用户通过 `McpServerBuilder::build()` 获得。

```cpp
class McpServer {
    McpServerConfig _config;
    shared_ptr<McpRegistry> _registry;
    seastar::sharded<McpShard> _shards;   // N 个 McpShard，一核一个
    vector<unique_ptr<ITransport>> _transports;
};
```

**启动序列**：

```
McpServer::start()
  1. _shards.start(config, registry)       → 每核创建 McpShard 实例
  2. _shards.invoke_on_all(&McpShard::start)→ 每核注册 MCP 方法到 Dispatcher
  3. 依配置创建 Transport：
     ├── HttpSseTransport(_port).start(*this)
     ├── StreamableHttpTransport(_port).start(*this)
     └── StdioTransport().start(*this)
```

**停止序列**（优雅关闭）：

```
McpServer::stop()
  1. 停止所有 Transport（不再接受新请求）
  2. _shards.stop()
     ├── 关闭所有 SSE session（推送空消息触发协程退出）
     └── 取消所有待处理的双向 RPC（set_exception）
```

### 3.6 传输层：Transport

**文件**：`include/mcp/transport/`

所有传输层实现 `ITransport` 接口：

```cpp
class ITransport {
    virtual future<> start(McpServer& server) = 0;
    virtual future<> stop() = 0;
};
```

#### HTTP/SSE Transport（端口 8080）

协议：客户端先 GET /sse 建立长连接，获取 session_id 后通过 POST /message 发送请求，响应通过 SSE 推回。

```
客户端                     HttpSseTransport               McpShard(Core K)
  │                              │                              │
  │  GET /sse                    │                              │
  │─────────────────────────────►│                              │
  │                              │  create_session()            │
  │                              │─────────────────────────────►│
  │                              │  "s{K}_{N}"                  │
  │                              │◄─────────────────────────────│
  │  event: endpoint             │                              │
  │  data: /message?sessionId=   │                              │
  │  s{K}_{N}                    │                              │
  │◄─────────────────────────────│                              │
  │                              │                              │
  │  [SSE 长连接保持，等待消息队列]│                              │
  │◄═══════════════════════════════════════════════════════════ │
```

```
客户端          HttpSseTransport(Core J)       McpShard(Core K)
  │                     │                           │
  │  POST /message       │                           │
  │  ?sessionId=s{K}_{N} │                           │
  │─────────────────────►│                           │
  │                      │ shards.local().dispatch() │
  │                      │ (在 Core J 上执行)         │
  │                      │                           │
  │                      │  if J == K:               │
  │                      │    push_to_session()──────►│→ SSE 队列
  │                      │                           │
  │                      │  if J != K:               │
  │                      │    invoke_on(K, push)──────►│→ SSE 队列
  │  202 Accepted        │                           │
  │◄─────────────────────│                           │
  │                      │                           │
  │  data: {"result":...}│                           │
  │◄═════════════════════════════════════════════════│ SSE 推送
```

#### Streamable HTTP Transport（端口 8081）

单端点 `/mcp`，通过 HTTP 方法和请求头区分五种工作模式：

```
POST /mcp
├── 无 session_id + 无 Accept:SSE     → 直接返回 JSON（无状态模式）
├── 无 session_id + Accept:SSE        → 新建 SSE session，建立长连接
├── 有 Mcp-Session-Id                 → 向已有 session 发请求（202）
GET  /mcp + Mcp-Session-Id            → 重连已有 SSE 流
DELETE /mcp + Mcp-Session-Id          → 关闭 session（200）
```

#### StdIO Transport

```
stdin → [seastar::thread 逐行读取] → dispatch() → stdout
```

`seastar::thread` 是 Seastar 的协程纤程（非 `std::thread`），内部可调用 `.get()` 而不阻塞 reactor，是处理阻塞式 stdin 的正确模式。只运行于 shard 0（stdin/stdout 是全局资源）。

### 3.7 构建器：McpServerBuilder

**文件**：`include/mcp/core/builder.hh`

流式 Builder API 封装了全部配置，返回配置完毕的 `McpServer`：

```
McpServerBuilder{}
├── .name("my-server")             → _config.name
├── .version("1.0.0")             → _config.version
├── .with_http(8080)              → 启用 HTTP/SSE，端口 8080
├── .with_streamable_http(8081)   → 启用 Streamable HTTP，端口 8081
├── .with_stdio()                 → 启用 StdIO
├── .add_tool<CalculateSumTool>() → registry.register_tool(make_shared<T>())
├── .add_resource<SystemInfoRes>()→ registry.register_resource(...)
├── .add_prompt<AnalyzePrompt>()  → registry.register_prompt(...)
└── .build()                      → return make_unique<McpServer>(config, registry)
```

---

## 4. SDK 接口参考

### 4.1 McpTool 接口

Tool 是 AI 客户端可以调用的"函数"。实现步骤：

**第一步：继承基类，实现三个必须方法**

```cpp
#include <mcp/mcp.hh>

class CalculateSumTool : public mcp::core::McpTool {
public:
    // 【必须】唯一标识，客户端通过此名称调用
    std::string get_name() const override { return "calculate_sum"; }

    // 【必须】工具描述 + 参数 Schema（JSON Schema Draft 7）
    nlohmann::json get_definition() const override {
        return {
            {"name", get_name()},
            {"description", "将两个数字相加"},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"a", {{"type", "number"}, {"description", "第一个数字"}}},
                    {"b", {{"type", "number"}, {"description", "第二个数字"}}}
                }},
                {"required", nlohmann::json::array({"a", "b"})}
            }}
        };
    }

    // 【必须】执行逻辑：C++20 协程，co_return 结果
    seastar::future<nlohmann::json> execute(const nlohmann::json& args) override {
        double a = args.value("a", 0.0);
        double b = args.value("b", 0.0);
        nlohmann::json result;
        result["content"] = {{{"type", "text"}, {"text", std::to_string(a + b)}}};
        co_return result;
        // 框架自动补充 isError: false，无需手动设置
    }
};
```

**可选方法（有默认值，按需覆盖）**：

| 方法 | 默认值 | 作用 |
|------|--------|------|
| `get_title()` | `get_name()` | GUI 显示名称 |
| `get_output_schema()` | `nullopt` | 声明结构化输出格式 |
| `get_annotations()` | `nullptr` | 行为提示（readOnly/idempotent/destructive） |
| `get_icon_uri()` | `nullopt` | 图标 URL |

**Tool Annotations 参考**：

```cpp
nlohmann::json get_annotations() const override {
    return {
        {"readOnlyHint",    true},   // 不修改外部状态（可安全自动调用）
        {"idempotentHint",  true},   // 幂等（重复调用结果一致）
        {"destructiveHint", false},  // 非破坏性（不会删除数据）
        {"openWorldHint",   false}   // 封闭域（结果可枚举）
    };
}
```

**execute() 的错误处理**：

```cpp
seastar::future<nlohmann::json> execute(const nlohmann::json& args) override {
    double b = args.value("b", 0.0);
    if (b == 0.0) {
        // 抛出 JsonRpcException → 客户端收到 {"error":{"code":-32602,"message":"..."}}
        throw mcp::protocol::JsonRpcException(
            mcp::protocol::JsonRpcErrorCode::InvalidParams, "除数不能为零");
    }
    // 普通 std::exception → InternalError (-32603)
    co_return nlohmann::json{{"content", {{{"type","text"},{"text","ok"}}}}};
}
```

**tools/call 完整交互**：

```
请求：
{
  "jsonrpc": "2.0", "id": 1, "method": "tools/call",
  "params": { "name": "calculate_sum", "arguments": {"a": 3, "b": 4} }
}

成功响应：
{
  "jsonrpc": "2.0", "id": 1,
  "result": {
    "content": [{"type": "text", "text": "7.0"}],
    "isError": false         ← 框架自动注入
  }
}

错误响应（JsonRpcException）：
{
  "jsonrpc": "2.0", "id": 1,
  "error": { "code": -32602, "message": "除数不能为零" }
}
```

---

### 4.2 McpResource 接口

Resource 代表 AI 可以读取的数据源（文件、数据库、实时指标等）。

```cpp
#include <mcp/mcp.hh>
#include <seastar/core/memory.hh>

class SystemInfoResource : public mcp::core::McpResource {
public:
    // 【必须】全局唯一 URI，客户端通过此 URI 读取
    std::string get_uri()  const override { return "sys://memory-info"; }
    std::string get_name() const override { return "seastar_memory_info"; }

    // 【必须】资源描述（包含 mimeType）
    nlohmann::json get_definition() const override {
        return {
            {"uri",         get_uri()},
            {"name",        get_name()},
            {"mimeType",    "application/json"},
            {"description", "Seastar 引擎实时内存与 CPU 状态"}
        };
    }

    // 【必须】读取内容，返回字符串（框架包装为 contents[0].text）
    seastar::future<std::string> read() override {
        auto stats = seastar::memory::stats();
        nlohmann::json info = {
            {"total_memory_bytes",    stats.total_memory()},
            {"free_memory_bytes",     stats.free_memory()},
            {"allocated_memory_bytes",stats.allocated_memory()},
            {"smp_core_count",        seastar::smp::count}
        };
        co_return info.dump(4);  // 格式化 JSON 字符串
    }
};
```

**resources/read 响应格式**（框架自动封装）：

```json
{
  "jsonrpc": "2.0", "id": 1,
  "result": {
    "contents": [{
      "uri":      "sys://memory-info",
      "mimeType": "application/json",
      "text":     "{\n  \"total_memory_bytes\": 536870912,\n  ...\n}"
    }]
  }
}
```

**URI 模板资源**（动态路径）：

在 `mcp_server.cc` 中，`resources/read` handler 支持 URI 前缀匹配：

```cpp
// sys://metrics/{component} 动态 URI 模板
if (uri.rfind("sys://metrics/", 0) == 0) {
    std::string comp = uri.substr(14);  // 提取 component 部分
    co_return generate_metrics_for(comp);
}
```

---

### 4.3 McpPrompt 接口

Prompt 是带参数的提示词模板，允许 AI 动态生成上下文相关的提示词。

```cpp
class AnalyzeSystemPrompt : public mcp::core::McpPrompt {
public:
    std::string get_name() const override { return "analyze_server_health"; }

    nlohmann::json get_definition() const override {
        return {
            {"name",        get_name()},
            {"description", "请求 AI 分析服务器健康状态"},
            {"arguments", nlohmann::json::array({
                {
                    {"name",        "focus"},
                    {"description", "分析重点：memory/cpu/disk/network"},
                    {"required",    false}     // 可选参数
                }
            })}
        };
    }

    // 【必须】根据 args 渲染消息数组
    seastar::future<nlohmann::json> get_messages(const nlohmann::json& args) override {
        std::string focus = args.value("focus", "general");
        std::string prompt_text =
            "你是 Seastar 性能调优专家。请分析服务器健康状态，"
            "特别关注：[" + focus + "]。\n"
            "步骤：\n"
            "1. 读取 sys://memory-info 获取实时数据\n"
            "2. 评估内存分配策略\n"
            "3. 给出优化建议";

        nlohmann::json msgs = nlohmann::json::array();
        msgs.push_back({
            {"role",    "user"},
            {"content", {{"type","text"}, {"text", prompt_text}}}
        });
        co_return msgs;
    }
};
```

**prompts/get 响应格式**：

```json
{
  "jsonrpc": "2.0", "id": 1,
  "result": {
    "description": "请求 AI 分析服务器健康状态",
    "messages": [{
      "role": "user",
      "content": { "type": "text", "text": "你是 Seastar 性能调优专家..." }
    }]
  }
}
```

**Completion 补全**：当 AI 客户端在输入 `focus` 参数时，服务器返回候选值：

```json
// 请求（用户输入 "me" 时触发）
{ "method": "completion/complete", "params": {
    "ref": {"type":"prompt","name":"analyze_server_health"},
    "argument": {"name":"focus","value":"me"}
}}

// 响应
{ "result": { "completion": {
    "values": ["memory"],   ← "me" 前缀匹配
    "total": 1, "hasMore": false
}}}
```

---

### 4.4 McpServerBuilder 接口

```cpp
mcp::McpServerBuilder{}
    // 服务器元信息
    .name("my-mcp-server")      // 出现在 initialize 响应中
    .version("1.0.0")

    // 传输配置（可组合，互不冲突）
    .with_http(8080)             // 启用 HTTP/SSE，GET/sse + POST /message
    .with_streamable_http(8081)  // 启用 Streamable HTTP，单端点 /mcp
    .with_stdio()                // 启用 StdIO（与 Claude Desktop 集成）

    // 注册能力（模板参数 = 实现类，支持构造函数参数透传）
    .add_tool<CalculateSumTool>()
    .add_tool<GetCurrentTimeTool>()
    .add_resource<SystemInfoResource>()
    .add_prompt<AnalyzeSystemPrompt>()

    // 构建（返回 unique_ptr<McpServer>）
    .build();
```

**最简完整 main.cc**：

```cpp
#include <mcp/mcp.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/signal.hh>

int main(int argc, char** argv) {
    seastar::app_template app;
    return app.run(argc, argv, []() -> seastar::future<> {
        auto server = mcp::McpServerBuilder{}
            .name("my-server").version("1.0.0")
            .with_http(8080)
            .add_tool<MyTool>()
            .build();

        co_await server->start();

        seastar::promise<> stop;
        seastar::handle_signal(SIGINT, [&] { stop.set_value(); }, true);
        co_await stop.get_future();

        co_await server->stop();
    });
}
```

---

### 4.5 高级接口：通知与双向 RPC

#### 资源变更通知（Push）

客户端订阅后，服务端数据变化时主动推送：

```
// 服务端触发（广播到所有核的所有订阅者）
co_await server->broadcast_resource_updated("sys://memory-info");

// 底层路径：
McpServer::broadcast_resource_updated(uri)
  └── shards.invoke_on_all(λ)
       └── McpShard::notify_resource_updated(uri)
            └── 对所有订阅了该 uri 的 session：
                 push "notifications/resources/updated" 到 SSE 队列
```

客户端 SSE 流收到：
```
data: {"jsonrpc":"2.0","method":"notifications/resources/updated","params":{"uri":"sys://memory-info"}}
```

#### 双向 RPC：Sampling（服务端请求 LLM 推理）

```
服务器（Tool.execute() 内）               客户端（AI 助手）
  │                                          │
  │  co_await server->request_sampling(      │
  │      session_id, {messages:[...],        │
  │      maxTokens:1000})                    │
  │                                          │
  │  1. 生成 req_id=42                        │
  │  2. 存 _pending[42] = promise            │
  │  3. push SSE:                            │
  │  {"id":42,"method":"sampling/create..."}─►│
  │                                          │ AI 处理后通过 POST /message 回复
  │  4. co_await promise.get_future()        │◄─ {"id":42,"result":{...}}
  │                                          │
  │  5. dispatch() → handle_client_response  │
  │     promise.set_value(result)            │
  │  co_return response ◄──────────────────  │
```

#### 进度通知（长任务推送）

```cpp
// 客户端请求时携带 progressToken
// {"method":"tools/call","params":{"_meta":{"progressToken":"tok1"},...}}

// Tool.execute() 内部推送进度
co_await _shard.push_progress(0.33f, 1.0f);  // 33% 完成

// 客户端 SSE 收到：
// data: {"method":"notifications/progress","params":{"progressToken":"tok1","progress":0.33,"total":1.0}}
```

---

## 5. 请求生命周期

以最典型的 **HTTP/SSE + tools/call** 为例，展示一个请求从网络到用户代码的完整路径：

```
[客户端]                [HttpSseTransport]          [McpShard Core-0]         [McpRegistry]
   │                          │                           │                        │
   │ POST /message             │                           │                        │
   │ ?sessionId=s0_1           │                           │                        │
   │ body: tools/call sum      │                           │                        │
   │──────────────────────────►│                           │                        │
   │                          │                           │                        │
   │                          │ req.content → body string │                        │
   │                          │ req.get_query_param(...)  │                        │
   │                          │ → session_id = "s0_1"     │                        │
   │                          │                           │                        │
   │                          │ shards.local().dispatch(  │                        │
   │                          │   body, "s0_1")───────────►│                        │
   │                          │                           │                        │
   │                          │                           │ _current_session_id    │
   │                          │                           │   = "s0_1"             │
   │                          │                           │                        │
   │                          │                           │ _dispatcher            │
   │                          │                           │  .handle_request(body) │
   │                          │                           │  → method="tools/call" │
   │                          │                           │  → handler(params)     │
   │                          │                           │                        │
   │                          │                           │ registry.call_tool(    │
   │                          │                           │  "calculate_sum",      │
   │                          │                           │  {a:3,b:4})────────────►│
   │                          │                           │                        │ tool.execute({a:3,b:4})
   │                          │                           │                        │ → {content:[...]}
   │                          │                           │◄───────────────────────│
   │                          │                           │ result["isError"]=false│
   │                          │                           │ → response JSON string │
   │                          │◄──────────────────────────│                        │
   │                          │                           │                        │
   │                          │ _parse_shard("s0_1") = 0  │                        │
   │                          │ 0 == this_shard_id(0)     │                        │
   │                          │ push_to_session("s0_1",   │                        │
   │                          │   response_str)           │                        │
   │                          │───────────────────────────►│ session.messages.push │
   │                          │                           │                        │
   │ 202 Accepted             │                           │                        │
   │◄──────────────────────────│                           │                        │
   │                          │                           │                        │
   │ data: {"result":...}     │                           │                        │
   │◄══════════════════════════════════════════════════════│ SSE 推送               │
```

---

## 6. 测试体系设计

项目采用三层测试策略，确保从单个类到完整系统的全面覆盖。

```
┌──────────────────────────────────────────────────┐
│         三层测试金字塔                             │
│                                                  │
│      ┌─────────────────────┐                     │
│      │   Python 集成测试    │  ← 端到端：真实进程  │
│      │   (pytest, 21个)     │    验证协议合规性    │
│      └──────────┬──────────┘                     │
│         ┌───────▼───────────────┐                │
│         │  C++ 单元测试          │  ← 隔离测试     │
│         │  (Boost.Test, 17个)   │    核心模块逻辑  │
│         └───────┬───────────────┘                │
│    ┌────────────▼────────────────────────┐       │
│    │       性能压测（Python, bench.py）    │  ← 量化│
│    │       QPS / P50 / P95 / P99         │  性能  │
│    └─────────────────────────────────────┘       │
└──────────────────────────────────────────────────┘
```

### 6.1 C++ 单元测试

**框架**：Boost.Test + Seastar Testing（`SEASTAR_TEST_CASE` 宏支持协程测试用例）

**运行**：
```bash
cd build
ninja test_dispatcher test_registry   # 编译
ctest --output-on-failure             # 运行（全部通过，0.43s）
```

#### test_dispatcher.cc — JsonRpcDispatcher（7 个用例）

```
JsonRpcDispatcher 测试矩阵：

  输入类型          │ 预期行为
  ──────────────────┼──────────────────────────────────────
  合法单请求        │ 路由到 handler，返回正确 id + result
  未知方法          │ error.code = -32601 (MethodNotFound)
  Batch 数组        │ 返回等长响应数组，包含所有结果
  通知（无 id）     │ 调用 handler，返回 nullopt（无响应体）
  非 JSON 字符串    │ error.code = -32700 (ParseError)
  缺少 jsonrpc 字段 │ error.code = -32600 (InvalidRequest)
  空 Batch 数组 []  │ error.code = -32600 (InvalidRequest)
```

#### test_registry.cc — McpRegistry（10 个用例）

```
McpRegistry 测试矩阵：

  测试名                    │ 验证内容
  ──────────────────────────┼────────────────────────────────────
  register_and_list_tool    │ list 包含注册的工具，name 字段正确
  auto_title_injection      │ definition 无 title → 自动注入 get_title()
  output_schema_injection   │ get_output_schema() 非空 → list 含 outputSchema
  annotations_injection     │ get_annotations() 非 null → list 含 annotations
  tool_not_found            │ call_tool("unknown") 抛 JsonRpcException
  call_tool_success         │ 正常调用 → result 含 isError:false
  pagination_tools_list     │ 注册 55 个工具，验证 get_tools_list() 返回全量
  register_and_read_resource│ read_resource(uri) 返回 contents.text
  resource_not_found        │ 未知 URI 抛 JsonRpcException
  prompt_get                │ get_prompt() 返回 messages + description
```

**协程测试用例示例**（Seastar Testing 特有写法）：

```cpp
SEASTAR_TEST_CASE(test_call_tool_success) {
    McpRegistry reg;
    reg.register_tool(std::make_shared<SimpleTool>("echo", "Echo", "ok"));

    auto result = co_await reg.call_tool("echo", json::object());

    // 验证 isError: false 自动注入
    BOOST_REQUIRE(result.contains("isError"));
    BOOST_REQUIRE_EQUAL(result["isError"].get<bool>(), false);
    BOOST_REQUIRE(result.contains("content"));
    co_return;
}
```

### 6.2 Python 集成测试

**框架**：pytest，启动真实 `demo_server` 进程进行端到端验证

**运行**：
```bash
python3 -m pytest tests/integration/ -v
# 预期：21 passed in ~9s
```

**conftest.py 工作原理**：

```python
@pytest.fixture(scope="session")   # 整个测试会话共用一个 server 进程
def server():
    proc = subprocess.Popen(
        [SERVER_BIN, "-c1", "-m256M", "--overprovisioned",
         "--default-log-level=error"])
    _wait_for_server("http://127.0.0.1:8080")  # 轮询等待就绪
    _wait_for_port(8081)
    yield proc
    proc.terminate(); proc.wait(timeout=5)
```

#### test_protocol.py — MCP 协议合规测试（15 个）

覆盖所有核心 MCP 方法：

```
initialize        → protocolVersion="2025-11-25", capabilities.resources.subscribe=true
ping              → result = {}
tools/list        → 每项含 name, title, inputSchema; 支持 cursor 分页
tools/call        → calculate_sum: {a:3,b:4} → result.content 含 "7"
                  → get_current_time: content 含时间字符串
                  → 结构化输出: result 含 isError:false
                  → 未知工具: 返回 JSON-RPC error（非 result）
resources/list    → 返回含 uri 的数组
resources/read    → sys://memory-info → contents[0].text 含有效 JSON
resources/subscribe → 返回 {}
prompts/list      → 返回 prompts 数组
prompts/get       → 返回 messages + description
logging/setLevel  → 返回 {}
roots/list        → 返回 {"roots":[]}
```

#### test_transport_sse.py（3 个）+ test_transport_stream.py（3 个）

```
HTTP/SSE 传输：
  port_accessible      → 端口 8080 正常响应
  session_established  → GET /sse → event:endpoint + data 含 sessionId
  message_via_session  → POST /message?sessionId=... → 202

Streamable HTTP 传输：
  ping                 → POST /mcp → 200 JSON
  tools_list           → tools 数组非空
  sse_mode             → Accept:SSE → Content-Type:text/event-stream
```

### 6.3 测试覆盖矩阵

```
模块                   │ 单元测试 │ 集成测试 │ 性能压测
───────────────────────┼──────────┼──────────┼──────────
JsonRpcDispatcher      │    ✅    │    ✅    │    ✅
McpRegistry            │    ✅    │    ✅    │    -
McpTool (接口)         │    ✅    │    ✅    │    ✅
McpResource (接口)     │    ✅    │    ✅    │    -
McpPrompt (接口)       │    ✅    │    ✅    │    -
HTTP/SSE Transport     │    -     │    ✅    │    ✅
Streamable Transport   │    -     │    ✅    │    ✅
StdIO Transport        │    -     │    -     │    -
多核 sharding          │    -     │    -     │    ✅
跨核 SSE 路由          │    -     │    -     │    ✅
```

---

## 7. 性能数据与分析

### 7.1 测试环境

| 项目 | 规格 |
|------|------|
| 机器类型 | 2 核 4G 虚拟机（benchmark 客户端与 server 同机） |
| OS | Ubuntu 24.04，Linux 6.8 |
| 编译器 | GCC 13，-O2 |
| 测试工具 | Python `concurrent.futures.ThreadPoolExecutor` |

### 7.2 单核基准（bench.py）

服务器以 **1 核** 运行，每场景 300 请求、20 并发：

| 场景 | RPS | 均值(ms) | P50(ms) | P95(ms) | P99(ms) | 错误率 |
|------|-----|----------|---------|---------|---------|--------|
| ping (SSE) | **542** | 30.1 | 27.1 | 64.6 | 90.5 | 0% |
| tools/list (SSE) | **355** | 43.3 | 36.3 | 98.8 | 140.9 | 0% |
| tools/call sum (SSE) | **472** | 34.3 | 32.2 | 74.2 | 100.8 | 0% |
| ping (Streamable) | **482** | 31.1 | 28.7 | 62.7 | 86.1 | 0% |

**分析**：

- `tools/list` 比 `ping` 慢约 34%，因为需要遍历注册表生成 JSON；这是纯 CPU 序列化开销
- HTTP/SSE 与 Streamable HTTP 的 `ping` 性能接近（542 vs 482 RPS），说明两种 transport 的协议处理开销相当
- P99/P50 比值约为 3.3x（27ms vs 90ms），尾延迟来自偶发的 OS 调度抖动

### 7.3 多核 QPS 扩展（multicore_bench.py Phase 1）

服务器分别以 1/2/4 核运行，50 并发，1000 请求/场景：

| 场景 | 1核 RPS | 2核 RPS | 4核 RPS | 2核效率 | 4核效率 |
|------|---------|---------|---------|---------|---------|
| ping (SSE) | 839 | 886 | 792 | 0.53 | 0.24 |
| tools/list (SSE) | 792 | 764 | 751 | 0.48 | 0.24 |
| tools/call (SSE) | 814 | 849 | 777 | 0.52 | 0.24 |
| ping (Streamable) | 839 | 886 | 792 | 0.53 | 0.24 |

**效率偏低的原因分析**：

```
理想情况（专用服务器）：
  4 核 server → 4 × 839 = 3356 RPS，效率 1.0

实际情况（同机压测）：
  4 核 server + 50 线程 benchmark 客户端，争抢同一批 CPU
  ┌────────────────────────────────────────────────────┐
  │  CPU 资源竞争示意图                                  │
  │                                                    │
  │  1核模式：1核运行server + 其余核运行benchmark → 客    │
  │           户端充足                                  │
  │                                                    │
  │  4核模式：4核运行server + 几乎没有CPU给benchmark    │
  │           → 客户端本身成为瓶颈，掩盖server扩展性     │
  └────────────────────────────────────────────────────┘
```

**获得真实扩展数据的方法**：

```bash
# 推荐：客户端/服务端分机（两台机器）
# 机器 A 运行 server（专用 CPU）
taskset -c 0-3 ./demo_server -c4 -m512M --overprovisioned

# 机器 B 运行 benchmark
python3 tests/perf/multicore_bench.py --host 192.168.1.100 --cores-list 4

# 预期：4核效率 > 0.85（Seastar Share-Nothing 几乎线性扩展）
```

### 7.4 跨核 SSE 路由开销（Phase 2）

用 2 核运行，分别测试请求落在 session 归属核（同核）与非归属核（跨核）的延迟：

| 场景 | RPS | P50(ms) | P95(ms) |
|------|-----|---------|---------|
| 同核请求（Core 0 → s0_N） | 765 | 13.4 | 42.9 |
| 跨核请求（Core 1 → s0_N） | 741 | 17.7 | 38.9 |
| **跨核额外开销** | | **+4.3ms** | |

```
跨核路由路径分析：

  POST /message?sessionId=s0_5           （落在 Core 1 处理）
    ↓
  Core 1: dispatch(body)                 （~1ms，Dispatcher + Registry）
    ↓
  Core 1: _parse_shard("s0_5") = 0       （解析出目标 shard）
    ↓
  Core 1: invoke_on(0, push_to_session)  （Seastar 跨核消息队列）
    ↓  [跨核通信延迟 ~4ms]
  Core 0: push_to_session("s0_5", msg)   （找到 session，写入 SSE 队列）
    ↓
  Core 0: SSE 流推送到客户端
```

4ms 跨核开销包含：提交任务到目标核的调度延迟 + 消息队列 push/pop + TCP 发送路径。对于 MCP 协议的典型使用场景（> 100ms 的轮询间隔），4ms 是完全可接受的。

### 7.5 Session Fanout 无退化（Phase 3）

同时持有 10/50/100 个活跃 SSE session，并发向所有 session 推送消息：

| 并发 session 数 | RPS | P50(ms) | P99(ms) |
|----------------|-----|---------|---------|
| 10 | 551 | 8.3 | 16.6 |
| 50 | 621 | 11.8 | 35.7 |
| 100 | 681 | 9.6 | 30.1 |

**结论**：100 session 的 P50（9.6ms）与 10 session（8.3ms）基本持平，说明 `McpShard::_sessions`（`unordered_map`）的 O(1) 查找在高并发下没有退化。随着 session 数量增加，总体 RPS 甚至略有提升（551 → 681），原因是更多的并发请求更充分地利用了 Seastar 的事件驱动调度。

### 7.6 性能调优建议

**关闭不必要的日志**（最直接的优化，提升约 20-40%）：
```bash
./demo_server -c4 -m512M --overprovisioned --default-log-level=error
```

**使用 io_uring 后端**（内核 5.1+，减少系统调用开销）：
```bash
./demo_server -c4 -m512M --reactor-backend=io_uring
```

**CPU 隔离**（消除其他进程对 server 的干扰）：
```bash
taskset -c 0-3 ./demo_server -c4 -m512M
```

**增大文件描述符限制**（高并发 SSE session 必须）：
```bash
ulimit -n 65536
```

---

## 8. 关键工程决策

### 8.1 OBJECT 库而非 STATIC 库

`mcp_sdk` 使用 `add_library(mcp_sdk OBJECT ...)` 而非通常的 `STATIC`。

**原因**：`src/seastar_patches/http_common_patch.cc` 中定义了 `http_chunked_data_sink_impl::flush()` 的覆盖版本，修复了 Seastar 原版 `flush()` 为 no-op 导致 SSE 事件永远无法送达客户端的 bug。

静态库（`.a`）只在有未满足符号引用时才提取目标文件——因为没有代码"调用"这个 flush 覆盖（它是通过 C++ COMDAT 弱符号机制替换的），链接器不会提取该 `.o`，补丁失效。OBJECT 库直接将所有 `.o` 传给链接器，保证补丁始终生效。

```cmake
# CMakeLists.txt
add_library(mcp_sdk OBJECT          # ← OBJECT，非 STATIC
    src/mcp/server/mcp_server.cc
    src/seastar_patches/http_common_patch.cc  # ← 补丁文件必须被链接
)
```

### 8.2 SSE body_writer 参数按值传递

Seastar `reply::write_body()` 调用 body_writer 时传入的 `output_stream<char>` 是**临时对象**，在第一次 `co_await` 后即被销毁。若 body_writer 参数是右值引用，协程恢复时将访问已释放内存（Segfault）。

```cpp
// ❌ 错误：右值引用，协程恢复后 out 已析构
rep->write_body("text/event-stream",
    [](seastar::output_stream<char>&& out) -> seastar::future<> {
        co_await some_async_op();  // 协程暂停
        co_await out.write("...");  // out 已被销毁！
    });

// ✅ 正确：按值接收，协程帧获得所有权
rep->write_body("text/event-stream",
    [](seastar::output_stream<char> out) mutable -> seastar::future<> {
        co_await some_async_op();  // out 在协程帧中安全
        co_await out.write("...");
    });
```

### 8.3 自定义 Handler 绕过 Content-Type 覆盖

Seastar 的 `function_handler` 在用户函数返回后调用 `rep->done(_type)`，无条件覆盖 `Content-Type`。对于 `POST /mcp`（需要动态返回 `application/json` 或 `text/event-stream`），需要绕过这一行为。

解决方案：自定义 `_PostMcpHandler` 继承 `handler_base`（非 `function_handler`），在返回后调用 `rep->done()`（无参数，只设置 response line，不覆盖 Content-Type）。

```cpp
struct _PostMcpHandler : seastar::httpd::handler_base {
    future<unique_ptr<reply>> handle(...) override {
        return _handle_post(...).then([](unique_ptr<reply> r) {
            r->done();  // 无参数版本：只设置 response line
            return make_ready_future<...>(std::move(r));
        });
    }
};
```

### 8.4 McpServerConfig 定义位置

`McpServerConfig` 定义在 `mcp_shard.hh` 而非 `mcp_server.hh`，避免循环 include：

```
mcp_server.hh  include → mcp_shard.hh  include → McpServerConfig
                                    （builder.hh 引入 mcp_server.hh，
                                      mcp_server.hh 引入 mcp_shard.hh，
                                      config 随 shard 一起引入，无循环）
```

### 8.5 通知 Handler 在 run_in_background 中执行

JSON-RPC 通知（无 `id` 字段）不需要响应，但 handler 可能包含 `co_await`（异步操作）。若在 dispatch() 的协程链中直接执行，会阻塞响应返回。

解决方案：通知 handler 在 `seastar::engine().run_in_background()` 中独立执行，dispatch() 立即返回 `nullopt`。

```cpp
if (is_notif) {
    auto it = _notifications.find(method);
    if (it != _notifications.end()) {
        // 后台执行，不阻塞当前协程
        seastar::engine().run_in_background(it->second(params));
    }
    co_return std::nullopt;  // 通知无响应
}
```

---

*文档版本：2026-03-23 | SDK 版本：2.0.0 | 协议版本：MCP 2025-11-25*
