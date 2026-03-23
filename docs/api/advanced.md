# 高级 API

本文档覆盖超出基本 Tool/Resource/Prompt 的高级功能：资源订阅通知、进度推送、日志推送、双向 RPC（Sampling/Elicitation）。

---

## 1. 资源订阅与变更通知

### 1.1 客户端订阅

客户端调用 `resources/subscribe`：
```json
{
  "jsonrpc": "2.0", "id": 1,
  "method": "resources/subscribe",
  "params": { "uri": "sys://memory-info" }
}
```

服务器将 `(uri → session_id)` 存入当前 `McpShard::_subscriptions`。

### 1.2 服务端触发通知

当资源数据变化时，在任意 Seastar 协程中调用：

```cpp
// 广播到所有核心上订阅了该 URI 的 session
co_await server->broadcast_resource_updated("sys://memory-info");
```

底层实现：
```
broadcast_resource_updated(uri)
  └── shards.invoke_on_all(shard → shard.notify_resource_updated(uri))
       └── 对本核订阅该 uri 的所有 session：
            push "notifications/resources/updated" 到 SSE 消息队列
```

客户端收到的 SSE 事件：
```
data: {"jsonrpc":"2.0","method":"notifications/resources/updated","params":{"uri":"sys://memory-info"}}
```

### 1.3 定时刷新示例

```cpp
// 在 main() 协程中，每 5 秒推送一次内存变化通知
seastar::engine().run_in_background(
    seastar::do_until(
        [&stop] { return stop; },
        [&server] {
            return seastar::sleep(std::chrono::seconds(5)).then([&server] {
                return server->broadcast_resource_updated("sys://memory-info");
            });
        }
    )
);
```

---

## 2. 进度通知（Progress Notification）

对于长时间运行的 Tool 调用，可以在 `execute()` 中途向客户端推送进度。

### 2.1 框架支持

`McpShard` 持有当前请求的 `_current_progress_token`（从请求 `params._meta.progressToken` 中提取），以及 `push_progress()` 方法：

```cpp
// McpShard::push_progress
seastar::future<> push_progress(float progress,
                                std::optional<float> total = std::nullopt);
```

### 2.2 在 Tool 中推送进度

> **注意**：Tool 的 `execute()` 运行在 `McpShard::dispatch()` 的上下文中，可通过 `shards.local()` 访问当前核的 shard。

```cpp
// 假设 Tool 通过构造注入了 McpShard 引用（高级用法）
seastar::future<nlohmann::json> execute(const nlohmann::json& args) override {
    // 阶段 1/3
    co_await do_step_1();
    co_await _shard.push_progress(0.33f, 1.0f);

    // 阶段 2/3
    co_await do_step_2();
    co_await _shard.push_progress(0.67f, 1.0f);

    // 阶段 3/3
    auto result = co_await do_step_3();
    co_await _shard.push_progress(1.0f, 1.0f);

    co_return result;
}
```

客户端收到的 SSE 事件：
```
data: {"jsonrpc":"2.0","method":"notifications/progress","params":{"progressToken":"tok1","progress":0.33,"total":1.0}}
```

---

## 3. 日志推送（Log Notification）

服务器可以通过 SSE 推送日志条目，客户端能够实时收到服务器内部日志。

### 3.1 设置日志级别

客户端可以调用 `logging/setLevel` 控制推送粒度：
```json
{
  "jsonrpc": "2.0", "id": 1,
  "method": "logging/setLevel",
  "params": { "level": "info" }
}
```

支持的级别（大小写不敏感）：`debug`、`trace`、`info`、`warning`/`warn`、`error`、`critical`/`alert`/`emergency`

### 3.2 服务端推送日志

```cpp
co_await server->broadcast_log_notification(
    "info",        // 级别
    "my-service",  // logger 名称
    {              // 日志数据（任意 JSON）
        {"message", "数据库连接池已就绪"},
        {"pool_size", 10}
    }
);
```

客户端收到：
```
data: {"jsonrpc":"2.0","method":"notifications/message","params":{"level":"info","logger":"my-service","data":{"message":"数据库连接池已就绪","pool_size":10}}}
```

---

## 4. 双向 RPC —— Sampling（采样）

Sampling 允许服务器向 AI 客户端发起 LLM 推理请求，实现"服务端工具调用 LLM"的嵌套模式。

### 4.1 发起采样请求

```cpp
// 在某个 Tool::execute() 或 Resource::read() 中
nlohmann::json sampling_params = {
    {"messages", nlohmann::json::array({
        {{"role", "user"}, {"content", {{"type", "text"}, {"text", "总结以下内容：..." }}}}
    })},
    {"maxTokens", 1000},
    {"systemPrompt", "你是一个摘要助手"}
};

// session_id 从 McpShard::_current_session_id 中取得（dispatch 时暂存）
nlohmann::json response = co_await server->request_sampling(session_id, sampling_params);
std::string generated_text = response["content"]["text"].get<std::string>();
```

### 4.2 内部机制

```
server.request_sampling(session_id, params)
  └── shards.local().send_request_to_client(session_id, "sampling/createMessage", params)
       ├── 生成唯一请求 ID
       ├── 构造 JSON-RPC request（含 id，发到 SSE 队列）
       ├── 存入 _pending_client_requests[id] = promise<json>
       └── co_await promise.get_future()
            ↑
客户端收到 SSE 后，向 POST /message 发送响应（无 method 字段）
    └── dispatch() → handle_client_response(json)
         └── 找到对应 promise，set_value(json["result"])
              └── promise 解析，co_await 返回
```

---

## 5. 双向 RPC —— Elicitation（信息收集）

Elicitation 允许服务器向用户请求额外信息（如确认操作、输入参数等）。

### 5.1 发起 Elicitation

```cpp
nlohmann::json elicitation_params = {
    {"message", "请确认要删除以下文件：/data/important.db"},
    {"requestedSchema", {
        {"type", "object"},
        {"properties", {
            {"confirmed", {{"type", "boolean"}}},
            {"reason",    {{"type", "string"}}}
        }},
        {"required", {"confirmed"}}
    }}
};

nlohmann::json user_input =
    co_await server->request_elicitation(session_id, elicitation_params);

if (user_input.value("confirmed", false)) {
    co_await delete_file("/data/important.db");
}
```

---

## 6. McpServer 广播接口汇总

```cpp
class McpServer {
    // 广播资源更新通知到所有订阅了该 URI 的 session
    seastar::future<> broadcast_resource_updated(const std::string& uri);

    // 广播日志通知到所有核心的所有活跃 session
    seastar::future<> broadcast_log_notification(
        const std::string& level,
        const std::string& logger_name,
        const nlohmann::json& data);

    // 向指定 session 发送 Sampling 请求并等待响应
    seastar::future<nlohmann::json> request_sampling(
        const std::string& session_id,
        const nlohmann::json& params);

    // 向指定 session 发送 Elicitation 请求并等待响应
    seastar::future<nlohmann::json> request_elicitation(
        const std::string& session_id,
        const nlohmann::json& params);
};
```

---

## 7. McpShard 底层接口

高级使用场景下直接操作 shard：

```cpp
// 获取当前 shard（在 Seastar 协程中）
auto& shard = server->shards().local();

// 推送消息到指定 session
co_await shard.push_to_session(session_id, json_string);

// 广播通知到本核所有 session
co_await shard.broadcast_notification(json_string);

// 触发订阅通知（本核）
co_await shard.notify_resource_updated(uri);

// 推送进度
co_await shard.push_progress(0.5f);

// 双向 RPC（本核）
auto resp = co_await shard.send_request_to_client(session_id, method, params);
```

---

## 8. 注意事项

1. **session_id 可用性**：进度通知和双向 RPC 需要 session_id，它只在有 SSE session 的连接中有效。StdIO 传输和无 session 的 Streamable HTTP 请求无法使用这些特性。
2. **双向 RPC 超时**：`send_request_to_client()` 会无限等待客户端响应。生产环境应包裹超时：
   ```cpp
   auto resp = co_await seastar::with_timeout(
       seastar::timer<>::clock::now() + std::chrono::seconds(30),
       shard.send_request_to_client(session_id, method, params)
   );
   ```
3. **跨核广播**：`broadcast_*` 系列方法使用 `shards.invoke_on_all()`，会向所有核心发消息。高频调用时注意跨核通信开销。
4. **订阅清理**：session 断开时，框架自动调用 `cleanup_subscriptions(session_id)` 清除该 session 的所有订阅。无需手动管理。
