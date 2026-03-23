# seastar-mcp-server Multi-core Benchmark

Generated: 2026-03-23 11:56:14

## QPS Scaling (cores × throughput)

| Scenario | Cores | RPS | Mean(ms) | P50(ms) | P95(ms) | P99(ms) | Eff | Err% |
|---|---|---|---|---|---|---|---|---|
| cores=1  ping   [SSE] | 1 | 557.2 | 72.09 | 62.95 | 163.04 | 241.85 | 1.000 | 0.0% |
| cores=1  tools/list [SSE] | 1 | 791.7 | 49.66 | 46.64 | 98.36 | 125.79 | 1.000 | 0.0% |
| cores=1  tools/call [SSE] | 1 | 813.5 | 50.6 | 47.38 | 105.34 | 141.91 | 1.000 | 0.0% |
| cores=1  ping   [Streamable] | 1 | 838.8 | 48.3 | 43.88 | 102.06 | 130.64 | 1.000 | 0.0% |
| cores=2  ping   [SSE] | 2 | 784.1 | 83.36 | 69.31 | 207.27 | 280.81 | 0.467 | 0.0% |
| cores=2  tools/list [SSE] | 2 | 763.7 | 84.91 | 74.85 | 197.05 | 270.23 | 0.482 | 0.0% |
| cores=2  tools/call [SSE] | 2 | 849.2 | 71.13 | 60.95 | 167.07 | 234.04 | 0.522 | 0.0% |
| cores=2  ping   [Streamable] | 2 | 885.9 | 70.77 | 62.11 | 167.91 | 237.1 | 0.528 | 0.0% |
| cores=4  ping   [SSE] | 4 | 781.3 | 72.29 | 55.69 | 204.01 | 279.52 | 0.233 | 0.0% |
| cores=4  tools/list [SSE] | 4 | 751.4 | 68.09 | 50.84 | 189.45 | 293.96 | 0.237 | 0.0% |
| cores=4  tools/call [SSE] | 4 | 777.4 | 66.35 | 52.82 | 177.76 | 239.96 | 0.239 | 0.0% |
| cores=4  ping   [Streamable] | 4 | 791.7 | 70.72 | 53.59 | 198.99 | 270.76 | 0.236 | 0.0% |
| cross-shard: POST→shard-0 sessions | 2 | 765.5 | 16.93 | 13.39 | 42.94 | 55.42 | — | 0.0% |
| cross-shard: POST→shard-1 sessions | 2 | 741.2 | 18.85 | 17.66 | 38.87 | 61.11 | — | 0.0% |
| session-fanout n=10 | 2 | 551.5 | 9.16 | 8.3 | 8.3 | 16.55 | — | 0.0% |
| session-fanout n=50 | 2 | 621.1 | 12.96 | 11.79 | 11.79 | 35.72 | — | 0.0% |
| session-fanout n=100 | 2 | 681.2 | 10.93 | 9.58 | 9.58 | 30.72 | — | 0.0% |

> **Eff** = actual_RPS / (N_cores × single-core_RPS). Ideal = 1.00.

