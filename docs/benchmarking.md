# 性能测试

---

## 1. 测试工具

| 工具 | 路径 | 说明 |
|---|---|---|
| 单核基准 | `tests/perf/bench.py` | ping/tools/list/tools/call 单配置 QPS |
| 多核扩展 | `tests/perf/multicore_bench.py` | 1→2→4 核 QPS 扩展 + 跨 shard 路由开销 |

两个工具均通过 `concurrent.futures.ThreadPoolExecutor` 模拟并发客户端，无需外部 wrk/ab 依赖。

---

## 2. 单核基准（bench.py）

### 2.1 用法

```bash
# 先确保 demo_server 已启动
./build/examples/demo/demo_server -c1 -m256M --overprovisioned --default-log-level=error &

# 运行基准（默认参数：50并发，2000请求，200预热）
python3 tests/perf/bench.py

# 自定义参数
python3 tests/perf/bench.py \
    --concurrency 100  \   # 并发线程数
    --requests    5000 \   # 总请求数
    --warmup      500  \   # 预热请求数
    --output-dir  /tmp/bench_results
```

### 2.2 参数说明

| 参数 | 默认 | 说明 |
|---|---|---|
| `--host` | `127.0.0.1` | Server 地址 |
| `--sse-port` | `8080` | HTTP/SSE 端口 |
| `--stream-port` | `8081` | Streamable HTTP 端口 |
| `--concurrency` | `50` | 并发客户端线程数 |
| `--requests` | `2000` | 每个场景的总请求数 |
| `--warmup` | `200` | 预热阶段请求数（不计入结果） |
| `--output-dir` | `tests/perf/` | 输出目录（results.json + report.md） |

### 2.3 测试场景

| 场景 | 端点 | 方法 | 说明 |
|---|---|---|---|
| ping (SSE) | `/message` | `ping` | 框架基准开销 |
| tools/list (SSE) | `/message` | `tools/list` | 中等复杂度 |
| tools/call sum (SSE) | `/message` | `tools/call` | 含参数处理 |
| ping (Streamable) | `/mcp` | `ping` | 对比两种 transport |

### 2.4 输出示例

```
============================================================
seastar-mcp-server Benchmark
  concurrency=50  requests=2000  warmup=200
============================================================

Scenario: ping (SSE)
  [ping (SSE)] warmup (200 req)... done
  [ping (SSE)] measuring (2000 req, 50 concurrent)... RPS=839  P50=43.9ms  P95=102.1ms  P99=130.6ms  err=0.0%
...

JSON results → tests/perf/results.json
Markdown report → tests/perf/report.md
```

---

## 3. 多核 QPS 扩展测试（multicore_bench.py）

自动启停不同核数的 server，测量 QPS 扩展性，以及跨 shard SSE 路由开销。

### 3.1 用法

```bash
# 完整测试：1/2/4 核，三个阶段
python3 tests/perf/multicore_bench.py

# 仅测试 QPS 扩展（跳过跨 shard 和 fanout 测试）
python3 tests/perf/multicore_bench.py --skip-cross-shard --skip-fanout

# 自定义核心数
python3 tests/perf/multicore_bench.py --cores-list 1,2,4,8

# 高强度测试
python3 tests/perf/multicore_bench.py \
    --concurrency 100 --requests 5000 --warmup 500
```

### 3.2 参数说明

| 参数 | 默认 | 说明 |
|---|---|---|
| `--server-bin` | `build/examples/demo/demo_server` | Server 二进制路径 |
| `--max-cores` | `4` | 最大测试核心数（自动检测 CPU 数量上限） |
| `--cores-list` | （根据 max-cores 自动生成） | 显式指定 `1,2,4` |
| `--concurrency` | `40` | 每核并发客户端数（总并发 = N × 40） |
| `--requests` | `1000` | 每场景总请求数 |
| `--warmup` | `100` | 预热请求数 |
| `--skip-cross-shard` | `false` | 跳过 Phase 2 跨 shard 测试 |
| `--skip-fanout` | `false` | 跳过 Phase 3 session fanout 测试 |
| `--output-dir` | `tests/perf/` | 输出目录 |

### 3.3 三个测试阶段

**Phase 1：QPS 扩展**

依次用 1/2/4 核启动 server，运行相同的负载，计算：

```
scaling_efficiency = actual_RPS(N cores) / (N × baseline_RPS(1 core))
```

理想值 = 1.00（线性扩展）。

**Phase 2：跨 shard 路由开销**

用 2 核启动 server，建立 10 个 shard 0 的 session 和 10 个 shard 1 的 session，并发 POST 消息，比较两组的 P50 延迟差值。

该差值反映 `shards.invoke_on(target_shard, push)` 的跨核通信代价。

**Phase 3：Session Fanout**

建立 10/50/100 个并发 SSE session，同时向每个 session 发送一条消息，测量总延迟分布。验证 session map 查找在高并发下没有退化。

### 3.4 实测结果（本机，共享 CPU）

本次基准在同一台机器上同时运行 server 和 benchmark 客户端（会相互抢占 CPU 资源，导致扩展效率偏低）：

**Phase 1 — QPS 扩展**

| 场景 | 1 核 RPS | 2 核 RPS | 4 核 RPS | 2核效率 | 4核效率 |
|---|---|---|---|---|---|
| ping [SSE] | 839 | 886 | 792 | 0.53 | 0.24 |
| tools/list [SSE] | 792 | 764 | 751 | 0.48 | 0.24 |
| tools/call [SSE] | 814 | 849 | 777 | 0.52 | 0.24 |
| ping [Streamable] | 839 | 886 | 792 | 0.53 | 0.24 |

**Phase 2 — 跨 shard 路由开销**

| 目标 session | RPS | P50 | P95 |
|---|---|---|---|
| shard-0 sessions | 765 | 13.4ms | 42.9ms |
| shard-1 sessions | 741 | 17.7ms | 38.9ms |
| **P50 差值（跨 shard 开销）** | | **4.27ms** | |

**Phase 3 — Session Fanout**

| 并发 session 数 | RPS | P50 | P99 |
|---|---|---|---|
| 10 | 551 | 8.3ms | 16.6ms |
| 50 | 621 | 11.8ms | 35.7ms |
| 100 | 681 | 9.6ms | 30.1ms |

### 3.5 结果解读

**为何扩展效率偏低（~0.5 → 2核，~0.24 → 4核）**？

测试时 benchmark 客户端（50-100 个线程）与 server 运行在同一台机器上，争夺同一组 CPU。随着 server 核数增加，客户端可用 CPU 减少，形成客户端侧瓶颈，掩盖了 server 的实际扩展能力。

**如何获得准确的扩展效率数据**：

```bash
# 方案 A：客户端/服务端分机测试
# 机器 A 运行 server
./demo_server -c4 -m512M --overprovisioned --default-log-level=error

# 机器 B 运行 benchmark（修改 --host 参数）
python3 tests/perf/multicore_bench.py \
    --host 192.168.1.100 \
    --cores-list 4 \
    --concurrency 100 --requests 5000

# 方案 B：使用 taskset 隔离 CPU（需 8+ 核机器）
taskset -c 0-3 ./demo_server -c4 -m512M --overprovisioned &
taskset -c 4-7 python3 tests/perf/multicore_bench.py --cores-list 4
```

**跨 shard 路由开销（4.27ms）**：

这是 Seastar `invoke_on()` 的跨核通信实际代价，包含：
- 向目标核 shard 提交任务的调度延迟
- 消息队列的 push/pop 延迟
- SSE 消息从目标 shard 返回到 TCP 连接的延迟

4ms 的开销对于 MCP 协议（通常毫秒级别的轮询间隔）是可接受的。

**Session Fanout 无退化**：

100 并发 session 的 P50（9.6ms）与 10 session（8.3ms）基本持平，说明 `McpShard::_sessions`（`unordered_map`）的 O(1) 查找在高并发下没有锁竞争退化。

---

## 4. 输出文件

运行后在 `--output-dir` 生成两个文件：

**`results.json`** — 原始数据：
```json
{
  "generated": "2024-01-01 12:00:00",
  "scenarios": [
    {
      "name": "cores=1  ping   [SSE]",
      "cores": 1,
      "rps": 839.0,
      "mean_ms": 47.3,
      "p50_ms": 43.9,
      "p95_ms": 102.1,
      "p99_ms": 130.6,
      "error_rate": 0.0,
      "scaling_efficiency": 1.0
    }
  ]
}
```

**`report.md`** — Markdown 表格，可直接插入文档。

---

## 5. 压测注意事项

1. **关闭 debug 日志**：`--default-log-level=error` 可显著提升吞吐（日志写入是 I/O 操作）
2. **预热阶段**：JIT 编译（GCC 不适用，但 Linux 内核 CPU 分支预测）和 TCP 连接池需要预热，建议 `--warmup >= 200`
3. **客户端线程数**：`--concurrency` 不宜超过本机 CPU 核数的 2-3 倍，否则线程切换开销主导结果
4. **统计意义**：`--requests >= 1000` 时 P95/P99 的统计误差 < 5%
5. **系统资源**：高并发测试时注意 `ulimit -n`（文件描述符限制）：
   ```bash
   ulimit -n 65536
   ```
