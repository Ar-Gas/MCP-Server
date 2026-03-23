# McpTool 接口

Tool 是 MCP 中可被 AI 客户端调用的"函数"。实现 `McpTool` 基类并通过 `add_tool<T>()` 注册即可。

**头文件**：`#include <mcp/core/interfaces.hh>`

---

## 基类定义

```cpp
namespace mcp::core {

class McpTool {
public:
    virtual ~McpTool() = default;

    // ── 必须实现 ────────────────────────────────────────────────────────────
    virtual std::string    get_name()       const = 0;   // 工具唯一名称
    virtual nlohmann::json get_definition() const = 0;   // 工具定义（含 inputSchema）
    virtual seastar::future<nlohmann::json>
        execute(const nlohmann::json& args) = 0;          // 执行逻辑（协程）

    // ── 可选覆盖（有默认实现）─────────────────────────────────────────────
    virtual std::string    get_title()        const { return get_name(); }
    virtual std::optional<nlohmann::json>
        get_output_schema()                   const { return std::nullopt; }
    virtual nlohmann::json get_annotations()  const { return nullptr; }
    virtual std::optional<std::string>
        get_icon_uri()                        const { return std::nullopt; }
};

} // namespace mcp::core
```

---

## 方法详解

### `get_name()` — 工具名称

全局唯一标识符，客户端调用时通过 `name` 字段指定。建议使用小写+下划线。

```cpp
std::string get_name() const override { return "calculate_sum"; }
```

### `get_title()` — 显示名称

展示给用户的可读名称，不参与路由。若不覆盖，默认返回 `get_name()`。

```cpp
std::string get_title() const override { return "计算两数之和"; }
```

### `get_definition()` — 工具定义

返回符合 MCP JSON Schema 的工具定义。`McpRegistry` 会自动将 `get_title()`、`get_output_schema()`、`get_annotations()`、`get_icon_uri()` 的返回值注入到此 JSON 中（无需手动写入）。

```cpp
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
            {"required", {"a", "b"}}
        }}
    };
}
```

**`inputSchema` 格式**：遵循 JSON Schema Draft 7，支持 `type`、`properties`、`required`、`enum`、`description` 等字段。

### `execute(args)` — 执行逻辑

C++20 协程，接收客户端传入的参数（已解析为 JSON），返回结果 JSON。

**返回格式（MCP 规范）**：

```json
{
  "content": [
    { "type": "text", "text": "结果字符串" }
  ]
}
```

`isError: false` 由 `McpRegistry::call_tool()` **自动注入**，无需手动设置。

出错时，抛出 `std::exception`，`dispatcher` 会将其转换为 JSON-RPC `InternalError`，或抛出 `JsonRpcException` 返回自定义错误码。

```cpp
seastar::future<nlohmann::json> execute(const nlohmann::json& args) override {
    double a = args.value("a", 0.0);
    double b = args.value("b", 0.0);
    nlohmann::json result;
    result["content"] = {{{"type", "text"}, {"text", std::to_string(a + b)}}};
    co_return result;
}
```

### `get_output_schema()` — 结构化输出 Schema

声明 `execute()` 返回值中结构化数据的 Schema（MCP 2024 扩展）。返回 `nullopt` 表示不声明。

```cpp
std::optional<nlohmann::json> get_output_schema() const override {
    return nlohmann::json{
        {"type", "object"},
        {"properties", {
            {"sum", {{"type", "number"}}}
        }},
        {"required", {"sum"}}
    };
}
```

### `get_annotations()` — 工具注解

声明工具的行为特征，帮助 AI 客户端决策是否自动调用。

```cpp
nlohmann::json get_annotations() const override {
    return {
        {"readOnlyHint",    true},   // 只读，不会修改外部状态
        {"idempotentHint",  true},   // 幂等，重复调用结果相同
        {"destructiveHint", false},  // 非破坏性
        {"openWorldHint",   false}   // 封闭域（结果可枚举）
    };
}
```

### `get_icon_uri()` — 图标 URI

可选的工具图标 URL，供 GUI 客户端展示。

```cpp
std::optional<std::string> get_icon_uri() const override {
    return "https://example.com/icons/calculator.png";
}
```

---

## 完整示例

### 示例 1：简单计算工具

```cpp
#include <mcp/core/interfaces.hh>

class AddTool : public mcp::core::McpTool {
public:
    std::string get_name()  const override { return "add"; }
    std::string get_title() const override { return "加法计算"; }

    nlohmann::json get_definition() const override {
        return {
            {"name", "add"},
            {"description", "两数相加"},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"a", {{"type", "number"}}},
                    {"b", {{"type", "number"}}}
                }},
                {"required", {"a", "b"}}
            }}
        };
    }

    seastar::future<nlohmann::json> execute(const nlohmann::json& args) override {
        double result = args["a"].get<double>() + args["b"].get<double>();
        co_return nlohmann::json{
            {"content", {{{"type", "text"}, {"text", std::to_string(result)}}}}
        };
    }
};
```

### 示例 2：异步 I/O 工具（协程）

```cpp
#include <mcp/core/interfaces.hh>
#include <seastar/net/dns.hh>

class DnsLookupTool : public mcp::core::McpTool {
public:
    std::string get_name() const override { return "dns_lookup"; }

    nlohmann::json get_definition() const override {
        return {
            {"name", "dns_lookup"},
            {"description", "DNS 解析"},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"hostname", {{"type", "string"}, {"description", "域名"}}}
                }},
                {"required", {"hostname"}}
            }}
        };
    }

    nlohmann::json get_annotations() const override {
        return {{"readOnlyHint", true}};
    }

    seastar::future<nlohmann::json> execute(const nlohmann::json& args) override {
        auto hostname = args["hostname"].get<std::string>();
        // co_await 真实的异步 DNS 查询
        auto addr = co_await seastar::net::dns::resolve_name(hostname);
        co_return nlohmann::json{
            {"content", {{{"type", "text"}, {"text", addr.as_sstring()}}}}
        };
    }
};
```

### 示例 3：带错误处理的工具

```cpp
#include <mcp/core/interfaces.hh>
#include <mcp/protocol/json_rpc.hh>

class DivTool : public mcp::core::McpTool {
public:
    std::string get_name() const override { return "divide"; }

    nlohmann::json get_definition() const override {
        return {
            {"name", "divide"},
            {"description", "除法，分母为零时返回 JSON-RPC 错误"},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"a", {{"type", "number"}}},
                    {"b", {{"type", "number"}}}
                }},
                {"required", {"a", "b"}}
            }}
        };
    }

    seastar::future<nlohmann::json> execute(const nlohmann::json& args) override {
        double b = args["b"].get<double>();
        if (b == 0.0) {
            // 抛出 JsonRpcException → 客户端收到 JSON-RPC error 响应
            throw mcp::protocol::JsonRpcException(
                mcp::protocol::JsonRpcErrorCode::InvalidParams,
                "division by zero"
            );
        }
        double result = args["a"].get<double>() / b;
        co_return nlohmann::json{
            {"content", {{{"type", "text"}, {"text", std::to_string(result)}}}}
        };
    }
};
```

---

## `tools/call` 请求/响应格式

**请求**：
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/call",
  "params": {
    "name": "calculate_sum",
    "arguments": { "a": 3, "b": 4 }
  }
}
```

**成功响应**：
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "content": [{ "type": "text", "text": "7.0" }],
    "isError": false
  }
}
```

**错误响应**（`JsonRpcException`）：
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": { "code": -32602, "message": "division by zero" }
}
```

---

## 注意事项

1. **`execute()` 必须是协程**：即使逻辑全同步，也需要有 `co_return`。如果调用了任何异步操作（文件、网络、定时器），用 `co_await`。
2. **不要阻塞 Seastar reactor**：`execute()` 运行在 Seastar 的协程调度器中。禁止调用 `std::this_thread::sleep_for`、同步文件 I/O 等阻塞操作（使用 `seastar::sleep` / `seastar::file` 替代）。
3. **参数校验**：`args` 来自客户端，建议使用 `.value("key", default)` 或 `.contains()` 进行防御性读取。
4. **线程安全**：`McpRegistry` 启动后只读，`execute()` 内部只访问自身成员（如有），无需加锁。
