# Transport 层

seastar-mcp-server 支持三种传输方式，可以通过 `McpServerBuilder` 同时启用多种。

---

## 1. HTTP/SSE Transport（端口 8080）

**类**：`mcp::transport::HttpSseTransport`
**文件**：`include/mcp/transport/http_sse_transport.hh`

符合 MCP 经典 HTTP+SSE 规范：客户端先建立 SSE 长连接获取 session_id，再通过 POST 发送请求。

### 1.1 端点

| 方法 | 路径 | 说明 |
|---|---|---|
| `GET` | `/sse` | 建立 SSE 长连接，接收推送 |
| `POST` | `/message` | 发送 JSON-RPC 请求 |
| `POST` | `/message?sessionId={id}` | 带 session 发送请求 |

### 1.2 连接流程

**步骤 1**：客户端建立 SSE 连接

```bash
curl -N -H "Accept: text/event-stream" http://127.0.0.1:8080/sse
```

服务器立即推送端点事件：
```
event: endpoint
data: /message?sessionId=s0_1
```

**步骤 2**：客户端通过 session_id 发送请求

```bash
curl -X POST "http://127.0.0.1:8080/message?sessionId=s0_1" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'
```

服务器向 SSE 流推送响应：
```
data: {"jsonrpc":"2.0","id":1,"result":{"tools":[...]}}
```

**步骤 3（可选）**：无 session 直接请求（用于初始化等不需要推送的场景）

```bash
curl -X POST "http://127.0.0.1:8080/message" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":0,"method":"ping","params":{}}'
# 响应：200 OK {"jsonrpc":"2.0","id":0,"result":{}}
```

### 1.3 Session ID 格式

```
s{shard_id}_{counter}

示例：s0_1   → shard 0，第 1 个 session
      s2_47  → shard 2，第 47 个 session
```

Session 归属于创建它的 CPU 核心（shard）。跨核消息路由通过 `shards.invoke_on(target)` 实现，对客户端透明。

### 1.4 响应状态码

| 状态码 | 含义 |
|---|---|
| `200 OK` | 无 session 请求，直接返回 JSON-RPC 响应 |
| `202 Accepted` | 有 session 请求，响应已推入 SSE 队列 |

### 1.5 多核行为

使用 `seastar::httpd::http_server_control`（内部是 `sharded<http_server>`），每个 CPU 核心独立接受 HTTP 连接。`GET /sse` 在哪个核被处理，session 就属于哪个核。

---

## 2. Streamable HTTP Transport（端口 8081）

**类**：`mcp::transport::StreamableHttpTransport`
**文件**：`include/mcp/transport/streamable_http_transport.hh`

符合 MCP 2024-11-05 Streamable HTTP 规范：单端点 `/mcp`，通过请求头和 Accept 字段区分不同模式。

### 2.1 端点

| 方法 | 路径 | 说明 |
|---|---|---|
| `POST` | `/mcp` | 多模式：直接响应 / SSE 流 / 已有 session |
| `GET`  | `/mcp` | 重连已有 SSE 流 |
| `DELETE` | `/mcp` | 关闭 session |

### 2.2 工作模式

**模式 A：无 session + 无 Accept 头 → 直接 JSON 响应**

```bash
curl -X POST http://127.0.0.1:8081/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"ping","params":{}}'
# 响应：200 application/json
# {"jsonrpc":"2.0","id":1,"result":{}}
```

**模式 B：无 session + Accept: text/event-stream → 新建 SSE 流**

```bash
curl -N -X POST http://127.0.0.1:8081/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: text/event-stream" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{}}}'
```

响应头包含：
```
Content-Type: text/event-stream
Mcp-Session-Id: sm0_1
```

响应体（SSE 流）：
```
data: {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-11-25",...}}

data: <后续服务器推送>
```

**模式 C：有 Mcp-Session-Id 头 → 向已有 session 发送请求**

```bash
curl -X POST http://127.0.0.1:8081/mcp \
  -H "Content-Type: application/json" \
  -H "Mcp-Session-Id: sm0_1" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{...}}'
# 响应：202 Accepted
# 结果通过 SSE 流推送
```

**模式 D：GET /mcp → 重连 SSE 流**

```bash
curl -N http://127.0.0.1:8081/mcp \
  -H "Mcp-Session-Id: sm0_1" \
  -H "Accept: text/event-stream"
# 重新接收该 session 的消息队列
```

**模式 E：DELETE /mcp → 关闭 session**

```bash
curl -X DELETE http://127.0.0.1:8081/mcp \
  -H "Mcp-Session-Id: sm0_1"
# 响应：200 OK
```

### 2.3 Session ID 格式

```
sm{shard_id}_{counter}

示例：sm0_1   → shard 0，第 1 个 Streamable HTTP session
```

解析函数同时支持 `sm{N}_...` 和 `s{N}_...` 格式（兼容 SSE transport）。

### 2.4 与 HTTP/SSE Transport 的对比

| 特性 | HTTP/SSE (`/sse`+`/message`) | Streamable HTTP (`/mcp`) |
|---|---|---|
| 连接端点 | 2 个（GET /sse + POST /message） | 1 个（POST /mcp） |
| Session 建立 | 先 GET /sse，再 POST /message | POST /mcp 携带 Accept header |
| 直接响应 | 不支持（无 session 直接响应） | 支持（无 session+无 Accept） |
| 重连 | 重新 GET /sse | GET /mcp + Mcp-Session-Id |
| 关闭 session | 客户端断开连接 | DELETE /mcp |
| 规范 | MCP 经典 | MCP 2024-11-05 |

---

## 3. StdIO Transport

**类**：`mcp::transport::StdioTransport`
**文件**：`include/mcp/transport/stdio_transport.hh`

从标准输入读取 JSON-RPC 请求，向标准输出写响应。每行一个 JSON 对象。

### 3.1 工作方式

```
stdin  →  [逐行读取]  →  dispatch()  →  stdout
```

内部使用 `seastar::thread`（非 `std::thread`）进行阻塞 I/O，不阻塞 Seastar reactor。只在 shard 0 上运行。

### 3.2 适用场景

- **Claude Desktop** / 其他 MCP 客户端以子进程方式启动 Server
- **调试/测试**：手动 echo JSON 行测试功能
- **管道集成**：`cat requests.jsonl | ./server | jq .`

### 3.3 使用方式

```bash
# 启动后手动输入（每行一个 JSON-RPC 请求）
./demo_server -c1 --overprovisioned
{"jsonrpc":"2.0","id":1,"method":"ping","params":{}}
# 输出：{"id":1,"jsonrpc":"2.0","result":{}}
```

### 3.4 与 Claude Desktop 集成

在 Claude Desktop 配置文件（`~/.claude_desktop_config.json`）中：

```json
{
  "mcpServers": {
    "my-server": {
      "command": "/path/to/demo_server",
      "args": ["-c1", "-m256M", "--overprovisioned", "--default-log-level=warn"]
    }
  }
}
```

---

## 4. 同时使用多种 Transport

可以同时启用所有三种传输，互不干扰：

```cpp
auto server = mcp::McpServerBuilder{}
    .with_http(8080)            // HTTP/SSE
    .with_streamable_http(8081) // Streamable HTTP
    .with_stdio()               // StdIO
    .add_tool<MyTool>()
    .build();
```

所有传输共享同一个 `McpRegistry`（工具/资源/Prompt），请求通过 `McpShard::dispatch()` 统一处理。

---

## 5. 技术细节：SSE 推送修复

Seastar 原版 `http_chunked_data_sink_impl::flush()` 继承了父类的空操作（no-op），导致 SSE 事件被缓冲在内核 TCP 缓冲区中，永远不发送到客户端。

SDK 通过 `src/seastar_patches/http_common_patch.cc` 提供了修复版本：

```cpp
virtual future<> flush() override {
    return _out.flush();  // 强制将 chunked 数据刷新到底层连接
}
```

为确保该补丁始终生效，`mcp_sdk` 使用 `OBJECT` 库（非 `STATIC`），所有目标文件直接传入链接器，不受静态库符号提取规则影响。
