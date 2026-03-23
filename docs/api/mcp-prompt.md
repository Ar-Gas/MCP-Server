# McpPrompt 接口

Prompt 是 MCP 中预设的提示词模板，AI 客户端可以通过参数获取定制化的系统提示词或用户消息。

**头文件**：`#include <mcp/core/interfaces.hh>`

---

## 基类定义

```cpp
namespace mcp::core {

class McpPrompt {
public:
    virtual ~McpPrompt() = default;

    // ── 必须实现 ────────────────────────────────────────────────────────────
    virtual std::string    get_name()       const = 0;   // Prompt 唯一名称
    virtual nlohmann::json get_definition() const = 0;   // Prompt 定义（含参数说明）
    virtual seastar::future<nlohmann::json>
        get_messages(const nlohmann::json& args) = 0;     // 生成消息列表

    // ── 可选覆盖 ─────────────────────────────────────────────────────────
    virtual std::string    get_title()      const { return get_name(); }
    virtual std::optional<std::string>
        get_icon_uri()                      const { return std::nullopt; }
};

} // namespace mcp::core
```

---

## 方法详解

### `get_name()` — Prompt 名称

全局唯一，客户端在 `prompts/get` 中通过 `name` 字段指定。

```cpp
std::string get_name() const override { return "analyze_server_health"; }
```

### `get_definition()` — Prompt 定义

包含名称、描述和参数列表（`arguments` 数组）。

```cpp
nlohmann::json get_definition() const override {
    return {
        {"name", get_name()},
        {"description", "分析服务器健康状态的提示词"},
        {"arguments", nlohmann::json::array({
            {
                {"name",        "focus"},
                {"description", "分析重点：memory / cpu / disk / network"},
                {"required",    false}
            },
            {
                {"name",        "severity"},
                {"description", "告警级别：info / warning / critical"},
                {"required",    false}
            }
        })}
    };
}
```

**参数字段说明**：
- `name`：参数名（客户端传入 `arguments` 对象的 key）
- `description`：参数含义（展示给用户）
- `required`：是否必须（`true` / `false`）

### `get_messages(args)` — 生成消息列表

协程，根据参数生成消息数组。消息格式遵循 MCP 规范：

```json
[
  {
    "role":    "user" | "assistant",
    "content": { "type": "text", "text": "..." }
  }
]
```

`prompts/get` 响应会在 `messages` 外包装 `description` 字段（从 `get_definition()["description"]` 取得）。

---

## 完整示例

### 示例 1：系统分析提示词

```cpp
#include <mcp/core/interfaces.hh>

class AnalyzeSystemPrompt : public mcp::core::McpPrompt {
public:
    std::string get_name()  const override { return "analyze_server_health"; }
    std::string get_title() const override { return "服务器健康分析"; }

    nlohmann::json get_definition() const override {
        return {
            {"name",        get_name()},
            {"description", "请求 AI 分析 Seastar 服务器健康状态和性能瓶颈"},
            {"arguments",   nlohmann::json::array({
                {{"name", "focus"}, {"description", "分析重点: memory/cpu/disk/network"}, {"required", false}}
            })}
        };
    }

    seastar::future<nlohmann::json> get_messages(const nlohmann::json& args) override {
        std::string focus = args.value("focus", "general");
        std::string text =
            "你是一个高级 C++ 和 Seastar 框架性能调优专家。\n"
            "请分析当前服务器的健康状态，特别关注：[" + focus + "]。\n\n"
            "操作步骤：\n"
            "1. 读取资源 sys://memory-info 获取实时内存和核心数据。\n"
            "2. 根据数据评估内存健康状况。\n"
            "3. 给出优化建议。";

        nlohmann::json msgs = nlohmann::json::array();
        msgs.push_back({
            {"role",    "user"},
            {"content", {{"type", "text"}, {"text", text}}}
        });
        co_return msgs;
    }
};
```

### 示例 2：代码审查提示词（多消息）

```cpp
#include <mcp/core/interfaces.hh>

class CodeReviewPrompt : public mcp::core::McpPrompt {
public:
    std::string get_name() const override { return "code_review"; }

    nlohmann::json get_definition() const override {
        return {
            {"name",        "code_review"},
            {"description", "代码审查提示词，包含系统提示和用户消息"},
            {"arguments",   nlohmann::json::array({
                {{"name", "language"}, {"description", "编程语言"}, {"required", true}},
                {{"name", "focus"},    {"description", "审查重点：security/performance/style"}, {"required", false}}
            })}
        };
    }

    seastar::future<nlohmann::json> get_messages(const nlohmann::json& args) override {
        std::string lang  = args.value("language", "C++");
        std::string focus = args.value("focus", "general");

        nlohmann::json msgs = nlohmann::json::array();

        // 系统提示（assistant 角色模拟系统提示）
        msgs.push_back({
            {"role", "assistant"},
            {"content", {
                {"type", "text"},
                {"text", "我是一个专业的 " + lang + " 代码审查员，擅长 " + focus + " 方面的分析。"}
            }}
        });

        // 用户请求
        msgs.push_back({
            {"role", "user"},
            {"content", {
                {"type", "text"},
                {"text", "请对以下 " + lang + " 代码进行审查，重点关注 " + focus + "：\n\n[粘贴代码]"}
            }}
        });

        co_return msgs;
    }
};
```

### 示例 3：无参数的简单提示词

```cpp
#include <mcp/core/interfaces.hh>

class WelcomePrompt : public mcp::core::McpPrompt {
public:
    std::string get_name() const override { return "welcome"; }

    nlohmann::json get_definition() const override {
        return {
            {"name",        "welcome"},
            {"description", "欢迎用户介绍服务器能力"},
            {"arguments",   nlohmann::json::array()}  // 无参数
        };
    }

    seastar::future<nlohmann::json> get_messages(const nlohmann::json&) override {
        co_return nlohmann::json::array({{
            {"role", "user"},
            {"content", {{"type", "text"}, {"text",
                "请介绍一下这个 MCP Server 提供了哪些工具和资源，"
                "以及如何使用它们来分析系统状态。"
            }}}
        }});
    }
};
```

---

## 自动补全支持

框架内置了 `completion/complete` 方法，可为 Prompt 参数提供候选值。默认实现在 `mcp_server.cc` 中，可扩展支持更多 Prompt：

```cpp
// mcp_server.cc 中示例：analyze_server_health 的 focus 参数补全
if (prompt_name == "analyze_server_health" && arg_name == "focus") {
    candidates = {"memory", "cpu", "disk", "network"};
    filtered = 以 arg_value 为前缀过滤后的候选列表;
}
```

**`completion/complete` 请求格式**：
```json
{
  "jsonrpc": "2.0", "id": 1,
  "method": "completion/complete",
  "params": {
    "ref":      { "type": "ref/prompt", "name": "analyze_server_health" },
    "argument": { "name": "focus", "value": "mem" }
  }
}
```

**响应**：
```json
{
  "result": {
    "completion": { "values": ["memory"], "hasMore": false }
  }
}
```

---

## `prompts/get` 请求/响应格式

**请求**：
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "prompts/get",
  "params": {
    "name":      "analyze_server_health",
    "arguments": { "focus": "memory" }
  }
}
```

**响应**：
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "description": "请求 AI 分析 Seastar 服务器健康状态和性能瓶颈",
    "messages": [
      {
        "role":    "user",
        "content": { "type": "text", "text": "你是一个高级 C++ ..." }
      }
    ]
  }
}
```

---

## 注意事项

1. **`get_messages()` 必须是协程**：即使内容是静态的也需要 `co_return`。
2. **参数容错**：用 `args.value("key", default_val)` 而不是 `args["key"]`，避免客户端未传参时 JSON 异常。
3. **消息数量无限制**：可以返回多条 `user` / `assistant` 交替消息，构成对话上下文。
4. **`description` 字段**：框架从 `get_definition()["description"]` 中取得，无需在 `get_messages()` 返回值中重复。
