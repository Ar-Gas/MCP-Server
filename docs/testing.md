# 测试指南

项目包含三层测试：C++ 单元测试、Python 集成测试、手动单机测试。

---

## 1. C++ 单元测试

### 1.1 测试文件

| 文件 | 测试对象 | 测试数量 |
|---|---|---|
| `tests/unit/test_dispatcher.cc` | `JsonRpcDispatcher` | 7 |
| `tests/unit/test_registry.cc` | `McpRegistry` | 10 |

### 1.2 构建

```bash
cd build
ninja test_dispatcher test_registry
```

### 1.3 运行

```bash
# 方式 A：通过 ctest（推荐）
ctest --output-on-failure -R "test_dispatcher|test_registry"

# 方式 B：直接运行可执行文件
./tests/test_dispatcher --log_level=test_suite --catch_system_errors=no \
    -- -c1 -m256M --overprovisioned --default-log-level=error

./tests/test_registry --log_level=test_suite --catch_system_errors=no \
    -- -c1 -m256M --overprovisioned --default-log-level=error
```

> **注意**：`--` 后面的参数传给 Seastar，前面的参数传给 Boost.Test。

### 1.4 测试覆盖范围

**test_dispatcher.cc** — `JsonRpcDispatcher` 测试：

| 测试名 | 验证内容 |
|---|---|
| `test_single_request` | 注册方法后正确路由，返回正确 `id` 和 `result` |
| `test_method_not_found` | 未注册方法返回 error code -32601 |
| `test_batch_request` | Batch 数组中两个请求均返回响应 |
| `test_notification_no_response` | 无 `id` 的通知返回 `nullopt` |
| `test_invalid_json` | 非 JSON 输入返回 ParseError (-32700) |
| `test_invalid_jsonrpc` | 缺少 `jsonrpc` 字段返回 InvalidRequest (-32600) |
| `test_batch_empty_array` | 空 Batch 数组返回 InvalidRequest |

**test_registry.cc** — `McpRegistry` 测试：

| 测试名 | 验证内容 |
|---|---|
| `test_register_and_list_tool` | 注册后 `get_tools_list()` 包含该工具 |
| `test_auto_title_injection` | definition 无 title 时自动注入 `get_title()` |
| `test_output_schema_injection` | `get_output_schema()` 非空时 list 包含 `outputSchema` |
| `test_annotations_injection` | `get_annotations()` 非 null 时 list 包含 `annotations` |
| `test_tool_not_found` | `call_tool("nonexistent")` 抛出 `JsonRpcException` |
| `test_call_tool_success` | 调用已注册工具，返回含 `isError: false` 的结果 |
| `test_pagination_tools_list` | 注册 > 50 工具，验证 cursor 分页（`nextCursor` 字段） |
| `test_register_and_read_resource` | 注册 resource，`read_resource(uri)` 返回正确内容 |
| `test_resource_not_found` | 未知 URI 抛出异常 |
| `test_prompt_get` | 已知 prompt 返回 messages + description |

---

## 2. Python 集成测试

集成测试使用 pytest，启动真实的 `demo_server` 进程进行端到端验证。

### 2.1 准备

```bash
# 1. 先编译 demo_server
cd build && ninja demo_server

# 2. 安装 Python 依赖
pip3 install pytest pytest-json-report requests
```

### 2.2 运行全部测试（推荐）

```bash
# 从项目根目录
python3 -m pytest tests/integration/ -v

# 或进入 integration 目录运行
cd tests/integration
python3 -m pytest . -v
```

**预期输出（21 个测试全部通过）**：
```
tests/integration/test_protocol.py::test_initialize               PASSED
tests/integration/test_protocol.py::test_ping                     PASSED
tests/integration/test_protocol.py::test_tools_list               PASSED
...
tests/integration/test_transport_sse.py::test_sse_session_established  PASSED
tests/integration/test_transport_stream.py::test_streamable_http_sse_mode  PASSED
============================== 21 passed in 9.20s ==============================
```

### 2.3 运行单个测试文件

```bash
python3 -m pytest tests/integration/test_protocol.py -v
python3 -m pytest tests/integration/test_transport_sse.py -v
python3 -m pytest tests/integration/test_transport_stream.py -v
```

### 2.4 运行单个测试用例

```bash
python3 -m pytest tests/integration/test_protocol.py::test_tools_call_calculate_sum -v
```

### 2.5 查看详细输出

```bash
python3 -m pytest tests/integration/ -v --tb=long -s
```

### 2.6 测试覆盖范围

**test_protocol.py** — MCP 协议合规测试（15 个）：

| 测试名 | 验证内容 |
|---|---|
| `test_initialize` | `protocolVersion == "2025-11-25"`，capabilities 含 `resources.subscribe` |
| `test_ping` | 返回空 result `{}` |
| `test_tools_list` | 每项含 `name`, `title`, `inputSchema` |
| `test_tools_list_pagination` | nextCursor 分页直到无 cursor |
| `test_tools_call_calculate_sum` | `{a:3,b:4}` → result text 含 "7" |
| `test_tools_call_get_current_time` | result text 含时间字符串 |
| `test_tools_call_structured_output` | result 含 `isError: false` |
| `test_tools_call_invalid_tool` | 未知 name → JSON-RPC error response |
| `test_resources_list` | 返回含 uri 的数组 |
| `test_resources_read` | 读取 sys://memory-info，返回 contents.text |
| `test_resources_subscribe` | 返回 `{}` |
| `test_prompts_list` | 返回 prompts 数组 |
| `test_prompts_get` | 返回 messages + description |
| `test_logging_setLevel` | `{level:"info"}` → `{}` |
| `test_roots_list` | 返回 `{"roots":[]}` |

**test_transport_sse.py** — HTTP/SSE 传输测试（3 个）：

| 测试名 | 验证内容 |
|---|---|
| `test_sse_port_accessible` | 端口 8080 可访问 |
| `test_sse_session_established` | GET /sse 输出含 `sessionId` 和 `event: endpoint` |
| `test_message_via_session_id` | 获取 session_id 后 POST /message → 202 |

**test_transport_stream.py** — Streamable HTTP 传输测试（3 个）：

| 测试名 | 验证内容 |
|---|---|
| `test_streamable_http_ping` | POST /mcp → 200 JSON response |
| `test_streamable_http_tools_list` | tools 数组非空 |
| `test_streamable_http_sse_mode` | Accept: text/event-stream → Content-Type: text/event-stream |

### 2.7 conftest.py 工作原理

测试套件使用 `session` 作用域的 fixture，整个测试会话共用一个 server 进程：

```python
@pytest.fixture(scope="session")
def server():
    proc = subprocess.Popen([SERVER_BIN, "-c1", "-m256M", "--overprovisioned", ...])
    _wait_for_server(SSE_BASE)     # 等待 8080 就绪
    _wait_for_port(8081)           # 等待 8081 就绪
    yield proc
    proc.terminate()
    proc.wait(timeout=5)
```

如果本地已有 `demo_server` 在运行，conftest 可能误判为自己启动的 server 就绪。运行测试前建议先确认没有残留进程：

```bash
pkill -f demo_server || true
```

---

## 3. 手动单机测试

无需编写测试代码，直接用 `curl` 和命令行工具验证 Server 功能。

### 3.1 启动 Server

```bash
./build/examples/demo/demo_server -c1 -m256M --overprovisioned --default-log-level=debug &
```

### 3.2 HTTP/SSE 传输测试

**测试 1：ping（无 session）**
```bash
curl -s -X POST http://127.0.0.1:8080/message \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"ping","params":{}}'
# 期望输出：{"id":1,"jsonrpc":"2.0","result":{}}
```

**测试 2：列出工具**
```bash
curl -s -X POST http://127.0.0.1:8080/message \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}' | python3 -m json.tool
```

**测试 3：调用工具 calculate_sum**
```bash
curl -s -X POST http://127.0.0.1:8080/message \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc":"2.0","id":1,"method":"tools/call",
    "params":{"name":"calculate_sum","arguments":{"a":10,"b":32}}
  }' | python3 -m json.tool
# 期望：result.content[0].text 包含 "42"
```

**测试 4：SSE 连接 + 消息路由**
```bash
# 终端 1：建立 SSE 连接
curl -N -s -H "Accept: text/event-stream" http://127.0.0.1:8080/sse

# 观察输出（记下 sessionId）：
# event: endpoint
# data: /message?sessionId=s0_1

# 终端 2：向 session 发送请求
curl -s -X POST "http://127.0.0.1:8080/message?sessionId=s0_1" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"ping","params":{}}'

# 终端 1 的 SSE 流会收到：
# data: {"id":1,"jsonrpc":"2.0","result":{}}
```

**测试 5：资源读取**
```bash
curl -s -X POST http://127.0.0.1:8080/message \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"resources/read","params":{"uri":"sys://memory-info"}}' \
  | python3 -m json.tool
```

**测试 6：Prompt 获取**
```bash
curl -s -X POST http://127.0.0.1:8080/message \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc":"2.0","id":1,"method":"prompts/get",
    "params":{"name":"analyze_server_health","arguments":{"focus":"memory"}}
  }' | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['result']['messages'][0]['content']['text'][:200])"
```

### 3.3 Streamable HTTP 传输测试

**测试 1：直接 JSON 响应**
```bash
curl -s -X POST http://127.0.0.1:8081/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}' \
  | python3 -c "import sys,json; d=json.load(sys.stdin); print('tools:', len(d['result']['tools']))"
```

**测试 2：SSE 模式（Accept: text/event-stream）**
```bash
curl -si -N -X POST http://127.0.0.1:8081/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: text/event-stream" \
  -d '{"jsonrpc":"2.0","id":1,"method":"ping","params":{}}' \
  --max-time 3
# 期望 Content-Type: text/event-stream 和 Mcp-Session-Id 头
```

**测试 3：使用 session**
```bash
# 获取 session_id（从上面的响应头中提取 Mcp-Session-Id）
SESSION_ID="sm0_1"

# 向 session 发送请求
curl -s -X POST http://127.0.0.1:8081/mcp \
  -H "Content-Type: application/json" \
  -H "Mcp-Session-Id: $SESSION_ID" \
  -d '{"jsonrpc":"2.0","id":2,"method":"ping","params":{}}'
# 响应：202 Accepted

# 关闭 session
curl -s -X DELETE http://127.0.0.1:8081/mcp \
  -H "Mcp-Session-Id: $SESSION_ID"
# 响应：200 OK
```

### 3.4 一键验证脚本

```bash
#!/bin/bash
# tests/manual_smoke_test.sh

SERVER_PID=""
cleanup() { [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null; }
trap cleanup EXIT

# 启动
./build/examples/demo/demo_server -c1 -m256M --overprovisioned \
    --default-log-level=error &
SERVER_PID=$!
sleep 2

PASS=0; FAIL=0
check() {
    local name=$1; shift
    if eval "$@" > /dev/null 2>&1; then
        echo "  [PASS] $name"; ((PASS++))
    else
        echo "  [FAIL] $name"; ((FAIL++))
    fi
}

check "ping SSE" \
    "curl -sf -X POST http://127.0.0.1:8080/message \
       -H 'Content-Type: application/json' \
       -d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"params\":{}}'"

check "tools/list SSE" \
    "curl -sf -X POST http://127.0.0.1:8080/message \
       -H 'Content-Type: application/json' \
       -d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}'"

check "ping Streamable" \
    "curl -sf -X POST http://127.0.0.1:8081/mcp \
       -H 'Content-Type: application/json' \
       -d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"params\":{}}'"

SSE_OUTPUT=$(curl -s --max-time 2 -H "Accept: text/event-stream" \
    http://127.0.0.1:8080/sse 2>/dev/null)
[[ "$SSE_OUTPUT" == *"sessionId"* ]] && { echo "  [PASS] SSE session"; ((PASS++)); } \
                                     || { echo "  [FAIL] SSE session"; ((FAIL++)); }

echo ""
echo "Results: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
```

```bash
chmod +x tests/manual_smoke_test.sh
./tests/manual_smoke_test.sh
```

---

## 4. 一键运行全部测试

```bash
# 先编译
cd build && ninja && cd ..

# 1. C++ 单元测试
ctest --test-dir build --output-on-failure -R "test_dispatcher|test_registry"

# 2. Python 集成测试
python3 -m pytest tests/integration/ -v

# 3. 可选：性能基准
python3 tests/perf/bench.py

# 4. 可选：多核 QPS 扩展测试
python3 tests/perf/multicore_bench.py --cores-list 1,2,4
```
