# 系统架构

---

## 1. 整体结构

```
┌─────────────────────────────────────────────────────────────────────┐
│                        应用层（用户代码）                             │
│   McpTool / McpResource / McpPrompt  +  McpServerBuilder            │
└────────────────────────────┬────────────────────────────────────────┘
                             │ build()
┌────────────────────────────▼────────────────────────────────────────┐
│                         McpServer                                   │
│   持有 sharded<McpShard>  +  vector<ITransport>                     │
└────────┬───────────────────┬──────────────────────┬────────────────┘
         │                   │                      │
  ┌──────▼──────┐   ┌────────▼───────┐   ┌─────────▼───────┐
  │HttpSseTrans │   │StreamableHttp  │   │  StdioTransport │
  │  port :8080 │   │Transport :8081 │   │  (shard 0 only) │
  └──────┬──────┘   └────────┬───────┘   └─────────┬───────┘
         │                   │                      │
         └───────────────────┴──────────────────────┘
                             │ dispatch(body, session_id)
         ┌───────────────────┼──────────────────────────────┐
         │                   │  sharded<McpShard>           │
  ┌──────▼──────┐   ┌────────▼──────┐            ┌──────────▼──────┐
  │  McpShard   │   │   McpShard    │   ......   │   McpShard      │
  │  core 0     │   │   core 1      │            │   core N-1      │
  │             │   │               │            │                 │
  │ Dispatcher  │   │  Dispatcher   │            │   Dispatcher    │
  │ Registry    │   │  Registry     │            │   Registry      │
  │ Sessions    │   │  Sessions     │            │   Sessions      │
  │ Subscript.  │   │  Subscript.   │            │   Subscript.    │
  └─────────────┘   └───────────────┘            └─────────────────┘
```

---

## 2. 模块说明

### 2.1 McpServer

**文件**：`include/mcp/server/mcp_server.hh`

- 持有 `seastar::sharded<McpShard>` —— 每个 CPU 核心一个 `McpShard` 实例
- 持有 `vector<unique_ptr<ITransport>>` —— 可同时挂载多个传输层
- 提供跨核广播接口：`broadcast_resource_updated(uri)` / `broadcast_log_notification()`
- 提供双向 RPC 入口：`request_sampling()` / `request_elicitation()`
- **不持有业务状态**，业务状态全部下沉到 `McpShard`

### 2.2 McpShard（核心隔离单元）

**文件**：`include/mcp/server/mcp_shard.hh`

Seastar `peering_sharded_service<McpShard>`，每个核心独立持有：

| 字段 | 类型 | 说明 |
|---|---|---|
| `_dispatcher` | `JsonRpcDispatcher` | JSON-RPC 方法路由 |
| `_registry` | `shared_ptr<McpRegistry>` | 工具/资源/Prompt 注册表（共享只读） |
| `_sessions` | `unordered_map<string, SseSession>` | 本核 SSE session 表 |
| `_subscriptions` | `unordered_map<uri, set<session_id>>` | 资源订阅表 |
| `_pending_client_requests` | `unordered_map<id, promise<json>>` | 双向 RPC 等待表 |
| `_current_session_id` | `string` | 当前正在处理的请求所属 session |

### 2.3 McpRegistry（注册表，共享只读）

**文件**：`include/mcp/core/registry.hh`

- 在 `McpServer` 启动前由 `McpServerBuilder` 填充
- 启动后只读，无需同步
- 所有 `McpShard` 共享同一个 `shared_ptr<McpRegistry>`

### 2.4 JsonRpcDispatcher

**文件**：`include/mcp/router/dispatcher.hh`

- 每个 `McpShard` 持有独立的 `Dispatcher`（方法表相同，执行上下文独立）
- 支持 JSON-RPC 2.0 单请求 + Batch 数组
- 通知（无 id）在 `run_in_background` 中执行，不阻塞响应

### 2.5 Transport 层

**文件**：`include/mcp/transport/*.hh`

- `ITransport` 纯虚接口，`start(McpServer&)` / `stop()`
- `HttpSseTransport`：使用 `http_server_control`（内部 `sharded<http_server>`），每核独立接受连接
- `StreamableHttpTransport`：同上，单端点 `/mcp` 处理所有模式
- `StdioTransport`：`seastar::thread` 阻塞读 stdin，只运行于 shard 0

---

## 3. 请求生命周期

### 3.1 HTTP/SSE —— 建立 SSE 连接

```
Client                  HttpSseTransport             McpShard (core K)
  │                           │                            │
  │ GET /sse                  │                            │
  │──────────────────────────►│                            │
  │                           │  create_session()          │
  │                           │───────────────────────────►│
  │                           │  session_id="s{K}_{N}"     │
  │                           │◄───────────────────────────│
  │ event: endpoint           │                            │
  │ data: /message?sessionId= │                            │
  │◄──────────────────────────│                            │
  │                    [SSE 长连接，等待消息队列]             │
```

### 3.2 HTTP/SSE —— POST 消息

```
Client            HttpSseTransport (core J)          McpShard (core K)
  │                       │                                │
  │ POST /message         │                                │
  │ ?sessionId=s{K}_{N}   │                                │
  │──────────────────────►│                                │
  │                       │ shards.local().dispatch()      │
  │                       │ (在 core J 执行)               │
  │                       │──────────────────────────────► │ (若 J==K)
  │                       │                                │
  │                       │ ──若 J≠K──                     │
  │                       │ shards.invoke_on(K, push)      │
  │                       │───────────────────────────────►│ push
  │                       │                                │
  │ 202 Accepted          │                                │
  │◄──────────────────────│                                │
  │                       │         [SSE 推送响应到客户端]  │
```

### 3.3 Streamable HTTP —— 无 session 直接响应

```
Client              StreamableHttpTransport
  │                         │
  │ POST /mcp               │
  │ (no Mcp-Session-Id)     │
  │ (no Accept: SSE)        │
  │────────────────────────►│
  │                         │ dispatch()
  │                         │──────►
  │                         │◄──────
  │ 200 application/json    │
  │ {"result": ...}         │
  │◄────────────────────────│
```

---

## 4. Share-Nothing 分片模型

Seastar 的 Share-Nothing 架构要求：
- **每个核心拥有独立的内存区域**，无需 mutex
- **跨核通信通过消息传递**：`seastar::smp::submit_to()` / `shards.invoke_on()`
- **Session 归属于创建它的核**：session_id 中编码 shard_id，路由时解析

```
Session ID: "s0_42"
               │  └── counter（本核内单调递增）
               └───── shard_id（创建 session 的核编号）

解析函数：_parse_shard("s0_42") → 0
           _parse_shard("s1_7")  → 1
           _parse_shard("sm2_3") → 2  (Streamable HTTP)
```

跨核 push 路径：
```cpp
unsigned target = _parse_shard(session_id);
if (target == seastar::this_shard_id()) {
    shards.local().push_to_session(session_id, msg);
} else {
    seastar::engine().run_in_background(
        shards.invoke_on(target, [session_id, msg](McpShard& s) {
            return s.push_to_session(session_id, std::move(msg));
        }));
}
```

---

## 5. 目录结构

```
seastar-mcp-server/
├── include/mcp/                  # 公开头文件（SDK 对外接口）
│   ├── mcp.hh                    # 单一入口 include
│   ├── core/
│   │   ├── interfaces.hh         # McpTool / McpResource / McpPrompt 基类
│   │   ├── registry.hh           # McpRegistry 统一注册表
│   │   └── builder.hh            # McpServerBuilder 流式 API
│   ├── protocol/
│   │   └── json_rpc.hh           # JSON-RPC 2.0 类型与错误码
│   ├── router/
│   │   └── dispatcher.hh         # JsonRpcDispatcher
│   ├── transport/
│   │   ├── transport.hh          # ITransport 接口 + SseSession
│   │   ├── stdio_transport.hh    # StdIO
│   │   ├── http_sse_transport.hh # HTTP/SSE
│   │   └── streamable_http_transport.hh  # Streamable HTTP
│   └── server/
│       ├── mcp_shard.hh          # McpShard（每核状态）
│       └── mcp_server.hh         # McpServer（顶层）
│
├── src/mcp/server/
│   └── mcp_server.cc             # MCP 方法注册与 McpShard 实现
│
├── src/seastar_patches/
│   └── http_common_patch.cc      # 修复 Seastar HTTP chunked flush bug
│
├── examples/demo/                # 完整示例应用
│   ├── main.cc
│   ├── tools/
│   ├── resources/
│   ├── prompts/
│   └── CMakeLists.txt
│
├── tests/
│   ├── unit/                     # C++ 单元测试（Boost.Test + Seastar testing）
│   ├── integration/              # Python 集成测试（pytest）
│   ├── perf/                     # 性能基准
│   └── CMakeLists.txt
│
├── docs/                         # 本文档目录
└── CMakeLists.txt
```

---

## 6. 关键设计决策

### 6.1 OBJECT 库而非 STATIC 库

`mcp_sdk` 使用 `add_library(mcp_sdk OBJECT ...)` 而非 `STATIC`。

原因：静态库（`.a`）的链接器只在有未满足符号引用时才提取目标文件（`.o`）。`http_common_patch.cc` 中定义的 `http_chunked_data_sink_impl::flush()` 覆盖了 Seastar 同名 COMDAT 弱符号，但如果链接器处理静态库时未遇到对该符号的引用，就不会提取该 `.o`，导致补丁失效。OBJECT 库直接传递所有目标文件给链接器，保证补丁始终生效。

### 6.2 SSE body_writer 参数按值传递

Seastar 的 `reply::write_reply()` 调用 body_writer 时传入的 `output_stream<char>` 是一个**临时对象**：

```cpp
// Seastar 内部（reply.cc）
_body_writer(http::internal::make_http_chunked_output_stream(out)).then([&out] {
    return out.write("0\r\n\r\n", 5);
});
```

这个临时对象在 body_writer 第一次 `co_await` 时就被销毁。如果 body_writer 参数是 `output_stream<char>&&`（右值引用），恢复执行时将访问已释放的内存 → Segfault。

修复：body_writer 参数改为 `output_stream<char>` **按值**接收，协程帧获得所有权：

```cpp
[...](seastar::output_stream<char> out) mutable -> seastar::future<> {
    // out 由协程帧持有，生命期贯穿整个 SSE 连接
}
```

### 6.3 Content-Type 不被 function_handler 覆盖

Seastar 的 `function_handler::handle()` 在用户函数返回后调用 `rep->done(_type)`，无条件覆盖 Content-Type。对于 POST /mcp 这种动态返回 JSON 或 SSE 的端点，需要绕过这一行为。

修复：使用自定义 `_PostMcpHandler` 继承 `handler_base`，在返回后调用 `rep->done()`（无参数，只设置 response line，不覆盖 Content-Type）。
