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
    McpServerBuilder& name(std::string n);
    McpServerBuilder& version(std::string v);

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

    // ── P0: 安全（默认即生效 / 可配置）────────────────────────
    McpServerBuilder& with_ip_whitelist(std::vector<std::string> cidrs);
    McpServerBuilder& with_ip_blacklist(std::vector<std::string> cidrs);
    McpServerBuilder& with_rate_limit(uint32_t rps, uint32_t burst = 0);
    McpServerBuilder& with_circuit_breaker(uint32_t failure_threshold = 5,
                                            uint32_t timeout_seconds = 30,
                                            uint32_t success_threshold = 2);
    McpServerBuilder& with_size_limits(size_t max_body_mb = 1,
                                        uint32_t max_batch = 20);

    // ── P1: 连接防护 + 审计日志 ──────────────────────────────
    McpServerBuilder& with_connection_limit(size_t max_per_ip = 10,
                                             size_t max_total = 10000);
    McpServerBuilder& with_audit_log(bool enable = true);

    // ── P2: 可选高级安全（默认 disabled）────────────────────────
    McpServerBuilder& with_api_key(std::vector<std::string> keys,
                                    std::string header = "X-API-Key");
    McpServerBuilder& with_tls(std::string cert_pem, std::string key_pem,
                                std::string ca_pem = "");

    // ── 构建 ─────────────────────────────────────────────────
    std::unique_ptr<server::McpServer> build();
};
```

---

## 基础配置

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

### `with_http(port)` / `with_streamable_http(port)` / `with_stdio()`

启用对应传输层，详见 [Transport 层文档](../transports.md)。

### `add_tool<T>()` / `add_resource<T>()` / `add_prompt<T>()`

注册组件，模板参数 `T` 为继承自基类的具体类，`args` 转发给其构造函数。

---

## 安全配置

> 完整安全设计详见 [安全防护文档](../security.md)。

### P0：请求体与 Batch 大小限制（默认生效）

```cpp
// 默认值：1MB body，20条 batch，无需显式调用
// 通过此方法调整：
builder.with_size_limits(2, 50);   // 放宽至 2MB body，50条 batch
builder.with_size_limits(512/1024.0, 10); // 收紧至 512KB，10条
```

- 超出 body 限制 → `413 Payload Too Large`
- 超出 batch 条数 → JSON-RPC `InvalidRequest` 错误

### P0：IP 白名单 / 黑名单

```cpp
// 白名单（非白即拒）：仅允许指定 CIDR 段访问
builder.with_ip_whitelist({"10.0.0.0/8", "192.168.0.0/16"});

// 黑名单（黑名单优先）：拒绝指定 CIDR 段，其余放行
builder.with_ip_blacklist({"1.2.3.4/32", "5.6.0.0/16"});

// 同时使用：黑名单先判，白名单后判
builder.with_ip_whitelist({"10.0.0.0/8"})
       .with_ip_blacklist({"10.0.1.0/24"});  // 内网但屏蔽某子网
```

- 拒绝时返回 `403 Forbidden`
- 支持 IPv4 CIDR 格式，如 `"192.168.1.1"` (= `/32`)
- IP 来源优先读 `X-Real-IP` → `X-Forwarded-For` → 默认放行

### P0：Per-IP 速率限制（令牌桶）

```cpp
builder.with_rate_limit(100);         // 每IP 100 req/s，突发 200（默认 2x）
builder.with_rate_limit(50, 500);     // 每IP 50 req/s，突发 500
```

| 参数 | 类型 | 说明 |
|------|------|------|
| `rps` | `uint32_t` | 稳态每秒请求数 |
| `burst` | `uint32_t` | 突发容量（0 = rps×2） |

- 超限时返回 `429 Too Many Requests`，附 `Retry-After: 1` header

### P0：Per-IP 熔断器

```cpp
builder.with_circuit_breaker();             // 默认：5次失败，30s，2次成功恢复
builder.with_circuit_breaker(10, 60);       // 10次失败，60s 冷却
builder.with_circuit_breaker(3, 15, 3);     // failure=3, timeout=15s, success=3
```

| 参数 | 类型 | 说明 |
|------|------|------|
| `failure_threshold` | `uint32_t` | 连续失败多少次触发熔断（默认 5） |
| `timeout_seconds` | `uint32_t` | OPEN 状态持续时间，之后转 HALF_OPEN（默认 30） |
| `success_threshold` | `uint32_t` | HALF_OPEN 时探测成功多少次才完全恢复（默认 2） |

- 熔断时返回 `503 Service Unavailable`
- "失败"定义：dispatch 返回包含 `"error"` 字段的 JSON-RPC 响应
- 状态机：`CLOSED → OPEN → HALF_OPEN → CLOSED`

### P1：连接数限制

```cpp
builder.with_connection_limit();             // 默认：每IP 10，总共 10000
builder.with_connection_limit(5, 1000);      // 每IP 5，总共 1000
```

- 仅对 SSE 长连接（GET /sse、POST /mcp with SSE）生效
- 超限时返回 `503 Service Unavailable`

### P1：安全审计日志

```cpp
builder.with_audit_log();      // 开启
builder.with_audit_log(false); // 关闭
```

开启后，所有被拒绝的请求写入 logger `mcp_security_audit`：

```
[AUDIT] shard=0 event=BLOCKED_IP ip=1.2.3.4 component=ip_filter
[AUDIT] shard=1 event=RATE_LIMITED ip=5.6.7.8 component=rate_limiter
[AUDIT] shard=0 event=CIRCUIT_OPEN ip=9.10.11.12 component=circuit_breaker
[AUDIT] shard=2 event=CONN_LIMIT_PER_IP ip=13.14.15.16 component=connection_limit
```

查看日志：
```bash
./my_server --logger-log-level mcp_security_audit=info ...
```

### P2：API Key 认证（disabled by default）

```cpp
// 启用后，每个请求必须携带有效 API Key，否则 401
builder.with_api_key({"secret-key-1", "secret-key-2"});

// 自定义 header 名（默认 X-API-Key）
builder.with_api_key({"token"}, "Authorization");
```

客户端发送方式（两种都支持）：
```bash
# X-API-Key header
curl -H "X-API-Key: secret-key-1" http://localhost:8080/message ...

# Authorization: Bearer
curl -H "Authorization: Bearer secret-key-1" http://localhost:8080/message ...
```

### P2：TLS 配置（disabled by default，transport 待实现）

```cpp
// 单向 TLS
builder.with_tls("server.crt", "server.key");

// 双向 mTLS（客户端证书认证）
builder.with_tls("server.crt", "server.key", "ca.crt");
```

> 注意：TLS 配置项已就绪，transport 层实现待后续版本完成。

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

            // ── 传输层 ──────────────────────────────────────────
            .with_http(8080)
            .with_streamable_http(8081)

            // ── P0 安全 ─────────────────────────────────────────
            .with_ip_whitelist({"10.0.0.0/8"})
            .with_rate_limit(200, 500)
            .with_circuit_breaker(5, 30)
            .with_size_limits(2, 30)

            // ── P1 安全 ─────────────────────────────────────────
            .with_connection_limit(20, 10000)
            .with_audit_log()

            // ── P2 安全（可选）──────────────────────────────────
            // .with_api_key({"prod-token-abc"})

            // ── 工具 / 资源 / Prompt ─────────────────────────────
            .add_tool<CalculateSumTool>()
            .add_tool<DatabaseQueryTool>("postgresql://localhost/mydb")
            .add_resource<SystemInfoResource>()
            .add_prompt<AnalyzeSystemPrompt>()
            .build();

        co_await server->start();

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

| 参数 | 说明 | 示例 |
|---|---|---|
| `-c N` | 使用 N 个 CPU 核心 | `-c 4` |
| `-m SIZE` | 分配内存大小 | `-m 512M` |
| `--overprovisioned` | 允许超额使用 CPU（共享机器必须开启） | |
| `--default-log-level=LEVEL` | Seastar 日志级别 | `--default-log-level=warn` |
| `--logger-log-level NAME=LEVEL` | 单独设置指定 logger 级别 | `--logger-log-level mcp_security_audit=info` |
| `--reactor-backend=BACKEND` | I/O 后端（epoll/io_uring） | `--reactor-backend=io_uring` |

```bash
# 生产环境：4核，限制512M，关闭多余日志，开启安全审计
./my_server -c4 -m512M --overprovisioned --default-log-level=warn \
            --logger-log-level mcp_security_audit=info

# 开发调试：单核，打开全部日志
./my_server -c1 -m256M --overprovisioned --default-log-level=debug
```
