# 安全防护指南

本文档覆盖 seastar-mcp-server SDK 内置安全层的完整设计、配置方法与使用建议。

---

## 1. 威胁模型

MCP Server 暴露 HTTP 端点接受 AI 客户端连接，面临以下主要威胁：

| 攻击面 | 具体威胁 | 对应防护 |
|--------|---------|---------|
| **网络层** | DDoS、IP 伪造、连接耗尽 | IP 过滤、连接数限制 |
| **协议层** | 超大 payload、Batch 炸弹、恶意 JSON | Body 大小限制、Batch 大小限制 |
| **业务层** | 高频调用、失控客户端、恶意参数 | 速率限制、熔断器 |
| **认证层** | 未授权访问、key 泄露 | API Key 认证（P2） |
| **传输层** | 中间人攻击、流量监听 | TLS/mTLS（P2，待实现） |

---

## 2. 安全优先级分级

### P0：基础安全（生产必须）

默认已启用部分，其余通过 Builder 一行开启：

| 功能 | 默认状态 | 开启方式 |
|------|---------|---------|
| 请求体大小限制（1 MB） | **始终生效** | `with_size_limits()` 调整 |
| Batch 大小限制（20 条） | **始终生效** | `with_size_limits()` 调整 |
| IP 白名单/黑名单 | disabled | `with_ip_whitelist()` / `with_ip_blacklist()` |
| Per-IP 速率限制 | disabled | `with_rate_limit()` |
| Per-IP 熔断器 | disabled | `with_circuit_breaker()` |

### P1：连接防护（推荐）

| 功能 | 默认状态 | 开启方式 |
|------|---------|---------|
| SSE 连接数限制 | disabled | `with_connection_limit()` |
| 安全审计日志 | disabled | `with_audit_log()` |

### P2：高级安全（可选）

| 功能 | 默认状态 | 开启方式 |
|------|---------|---------|
| API Key / Bearer Token 认证 | disabled | `with_api_key()` |
| TLS / mTLS | disabled | `with_tls()`（config 就绪，transport 待实现） |

---

## 3. P0：请求体与 Batch 大小限制

### 工作原理

在 transport 层最前端检查，**在 JSON 解析之前**拦截：

- **Body 大小**：`POST /message` 和 `POST /mcp` 读取 body 后立即检查字节数
- **Batch 大小**：`JsonRpcDispatcher::handle_request()` 中检查数组长度

### 配置

```cpp
// 默认值（即使不调用此方法也会生效）
builder.with_size_limits(1, 20);   // 1MB body，20条 batch

// 调整（如服务高吞吐场景）
builder.with_size_limits(4, 50);   // 4MB body，50条 batch
```

### 错误响应

```http
HTTP/1.1 413 Payload Too Large
{"error":"Payload Too Large","code":413}

// Batch 过大时（JSON-RPC 错误格式）
{"jsonrpc":"2.0","id":null,"error":{"code":-32600,"message":"Batch too large: max 20 requests per batch"}}
```

---

## 4. P0：IP 白名单 / 黑名单

### 工作原理

基于 IPv4 CIDR 段匹配（`(ip & mask) == network`），检查顺序：
1. 黑名单优先匹配（命中 → 拒绝）
2. 白名单匹配（白名单非空时：命中 → 放行，未命中 → 拒绝）
3. 默认策略（`default_allow = true/false`）

### IP 来源提取

```
请求到达时，依次尝试：
1. X-Real-IP header（nginx/envoy 标准设置）
2. X-Forwarded-For header 的第一个 IP
3. 空串 → 按 default_allow 策略处理（白名单模式下放行）
```

> 直连场景下建议在 nginx 中设置 `proxy_set_header X-Real-IP $remote_addr;`

### 配置示例

```cpp
// 场景1：仅限内网访问
builder.with_ip_whitelist({"10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"});

// 场景2：公网访问，但屏蔽已知恶意 IP
builder.with_ip_blacklist({"1.2.3.0/24", "5.6.7.8/32"});

// 场景3：内网访问 + 屏蔽内部特定子网
builder.with_ip_whitelist({"10.0.0.0/8"})
       .with_ip_blacklist({"10.0.1.0/24"});  // 屏蔽 10.0.1.x 子网

// 场景4：测试时放行所有（默认行为，不配置即可）
// 不调用 with_ip_whitelist/blacklist
```

### CIDR 格式规则

- 完整 IP：`"192.168.1.100"` 等价于 `"192.168.1.100/32"`
- A 类网段：`"10.0.0.0/8"` 匹配 10.x.x.x
- B 类网段：`"172.16.0.0/12"` 匹配 172.16.x.x ~ 172.31.x.x
- C 类网段：`"192.168.1.0/24"` 匹配 192.168.1.x
- 全放行（不建议）：`"0.0.0.0/0"`

---

## 5. P0：Per-IP 速率限制（令牌桶）

### 算法

令牌桶（Token Bucket）：
- 每个 IP 独立维护一个令牌桶
- 初始令牌数 = burst_size（允许启动时的突发流量）
- 每次请求消耗 1 个令牌
- 令牌按 `requests_per_second` 的速率持续补充（纳秒级精度）
- 令牌数不超过 burst_size

### 配置

```cpp
builder.with_rate_limit(rps, burst);

// 示例
builder.with_rate_limit(100);        // 100 req/s，突发 200（默认 2x）
builder.with_rate_limit(50, 500);    // 50 req/s，允许短暂 500 的突发
builder.with_rate_limit(1000, 2000); // 高吞吐服务
```

### 多核注意事项

每个 shard 独立持有限流器，同一 IP 的请求分散到 N 个 shard 时，**实际有效 RPS = 配置值 × N**。

例如：4 核环境下 `with_rate_limit(100)` 实际允许 ~400 req/s（每核 100）。这是 Seastar Share-Nothing 架构的正常权衡——避免跨核状态同步的开销。

如需严格限制，可将 `rps` 配置为目标值 ÷ 核心数。

### 错误响应

```http
HTTP/1.1 429 Too Many Requests
Retry-After: 1
{"error":"Too Many Requests","code":429}
```

---

## 6. P0：Per-IP 熔断器

### 状态机

```
            连续失败 ≥ failure_threshold
CLOSED ──────────────────────────────────────► OPEN
  ▲                                             │
  │ 连续成功 ≥ success_threshold               │ 等待 timeout_seconds
  │                                             │
HALF_OPEN ◄──────────────────────────────────── ┘
```

| 状态 | 含义 | 转换条件 |
|------|------|---------|
| `CLOSED` | 正常，放行所有请求 | 连续失败 ≥ `failure_threshold` → OPEN |
| `OPEN` | 熔断，拒绝所有请求（503） | 等待 `timeout_seconds` 秒 → HALF_OPEN |
| `HALF_OPEN` | 探测，放行少量请求 | 连续成功 ≥ `success_threshold` → CLOSED；任意失败 → OPEN |

### "失败"的定义

dispatch 调用返回后，transport 检查响应：
```cpp
bool is_error = response.find("\"error\"") != std::string::npos;
shard.record_outcome(client_ip, is_error);
```

- JSON-RPC error 响应（Parse error、Method not found、Tool 抛异常等）→ 记为失败
- 正常 JSON-RPC result 响应 → 记为成功
- 客户端恶意发送大量无效请求会触发熔断，保护服务端

### 配置

```cpp
// 默认值：5 次连续失败熔断，30s 后探测，2 次成功恢复
builder.with_circuit_breaker();

// 严格模式（AI 客户端不稳定时）
builder.with_circuit_breaker(3, 60, 3);  // 3次失败，60s 冷却，3次成功恢复

// 宽松模式（开发/测试环境）
builder.with_circuit_breaker(20, 10);    // 20次失败才熔断，10s 快速恢复
```

### 与速率限制的配合

速率限制和熔断器互补：
- **速率限制**：限制请求频率，防止资源耗尽
- **熔断器**：隔离持续产生错误的 IP，防止错误扩散

建议两者同时启用：
```cpp
builder.with_rate_limit(100, 200)
       .with_circuit_breaker(5, 30);
```

---

## 7. P1：连接数限制

### 适用范围

仅对建立 SSE 长连接时有效（`GET /sse` 和 `POST /mcp` with `Accept: text/event-stream`）。普通请求/响应模式不受影响。

### 配置

```cpp
builder.with_connection_limit(max_per_ip, max_total);

// 示例
builder.with_connection_limit();           // 默认：每IP 10，总共 10000
builder.with_connection_limit(5, 2000);    // 每IP 5，总共 2000
```

### 多核注意事项

同速率限制，per-IP 计数是 per-shard 的。4 核下每 IP 实际最多 `max_per_ip × 4` 个连接。

### 错误响应

```http
HTTP/1.1 503 Service Unavailable
{"error":"Service Unavailable","code":503}
```

---

## 8. P1：安全审计日志

### 配置

```cpp
builder.with_audit_log();  // 开启（默认关闭）
```

### 日志格式

所有被拒绝的请求写入 Seastar logger `mcp_security_audit`：

```
INFO  [mcp_security_audit] [AUDIT] shard=0 event=BLOCKED_IP ip=1.2.3.4 component=ip_filter
INFO  [mcp_security_audit] [AUDIT] shard=1 event=UNAUTHORIZED ip=5.6.7.8 component=api_key
INFO  [mcp_security_audit] [AUDIT] shard=0 event=RATE_LIMITED ip=9.0.0.1 component=rate_limiter
INFO  [mcp_security_audit] [AUDIT] shard=2 event=CIRCUIT_OPEN ip=10.0.0.5 component=circuit_breaker
INFO  [mcp_security_audit] [AUDIT] shard=0 event=CONN_LIMIT_PER_IP ip=192.168.1.1 component=connection_limit
INFO  [mcp_security_audit] [AUDIT] shard=0 event=CONN_LIMIT_TOTAL ip=172.16.0.1 component=connection_limit
```

### 启用日志输出

```bash
# 启动时单独开启 security audit logger
./my_server -c4 -m512M \
  --default-log-level=warn \
  --logger-log-level mcp_security_audit=info

# 如需写入文件（配合 shell 重定向）
./my_server ... --logger-log-level mcp_security_audit=info 2>> /var/log/mcp-security.log
```

---

## 9. P2：API Key 认证

### 配置

```cpp
builder.with_api_key({"key1", "key2", "key3"});

// 自定义 header（默认 X-API-Key）
builder.with_api_key({"token"}, "Authorization");
```

### 客户端使用方式

SDK 自动识别两种方式，无需配置：

```bash
# 方式1：X-API-Key header
curl -H "X-API-Key: key1" http://localhost:8080/message \
     -d '{"jsonrpc":"2.0","id":1,"method":"ping","params":{}}'

# 方式2：Authorization: Bearer
curl -H "Authorization: Bearer key1" http://localhost:8080/message \
     -d '{"jsonrpc":"2.0","id":1,"method":"ping","params":{}}'
```

### 错误响应

```http
HTTP/1.1 401 Unauthorized
WWW-Authenticate: Bearer
{"error":"Unauthorized","code":401}
```

### 安全建议

- Key 应足够随机（建议 ≥ 32 字节随机字符串）
- 不要在配置文件或代码中硬编码 key，使用环境变量注入：
  ```cpp
  std::string key = std::getenv("MCP_API_KEY") ?: "";
  builder.with_api_key({key});
  ```

---

## 10. P2：TLS 配置（待实现）

TLS 配置项已就绪，可通过 Builder 设置，后续版本将与 Seastar TLS 完成集成：

```cpp
// 单向 TLS
builder.with_tls("server.crt", "server.key");

// 双向 mTLS（验证客户端证书）
builder.with_tls("server.crt", "server.key", "ca.crt");
```

---

## 11. 生产环境推荐配置

### 内网部署（AI 客户端在受控内网）

```cpp
mcp::McpServerBuilder{}
    .name("internal-mcp")
    .with_ip_whitelist({"10.0.0.0/8", "192.168.0.0/16"})
    .with_rate_limit(500, 1000)
    .with_circuit_breaker(5, 30)
    .with_connection_limit(50, 10000)
    .with_audit_log()
    .with_http(8080)
    .build()
```

### 公网部署（AI SaaS 场景）

```cpp
mcp::McpServerBuilder{}
    .name("public-mcp")
    .with_rate_limit(50, 100)         // 严格限速
    .with_circuit_breaker(3, 60)      // 更快熔断
    .with_connection_limit(5, 1000)   // 严格连接限制
    .with_api_key({"prod-token-xxx"}) // 必须认证
    .with_audit_log()
    .with_size_limits(1, 10)          // 收紧 body 和 batch
    // .with_tls("cert.pem", "key.pem")  // 待实现
    .with_http(8080)
    .build()
```

### 本地开发/测试

```cpp
mcp::McpServerBuilder{}
    .name("dev-mcp")
    // 不配置任何安全策略，或只保留默认的 size limits
    .with_http(8080)
    .build()
```

---

## 12. 检查链执行顺序

每个 HTTP 请求在 transport 层按以下顺序执行安全检查（任意步骤失败即短路返回）：

```
① Body 大小检查       → 失败: 413
② IP 黑名单匹配       → 失败: 403
③ IP 白名单匹配       → 失败: 403
④ API Key 验证 (P2)   → 失败: 401
⑤ 速率限制令牌消耗    → 失败: 429
⑥ 熔断器状态检查      → 失败: 503
⑦ SSE 连接数检查 (P1) → 失败: 503
────────────────────────────────
  McpShard::dispatch()  正常执行
────────────────────────────────
⑧ 记录 RPC 结果 → 更新熔断器状态
```
