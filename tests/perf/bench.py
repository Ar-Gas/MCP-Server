#!/usr/bin/env python3
"""
seastar-mcp-server Performance Benchmark
=========================================
Uses concurrent.futures.ThreadPoolExecutor to simulate concurrent clients.

Usage:
    python tests/perf/bench.py [options]

Options:
    --host          Server host (default: 127.0.0.1)
    --sse-port      HTTP+SSE transport port (default: 8080)
    --stream-port   Streamable HTTP transport port (default: 8081)
    --concurrency   Number of concurrent threads (default: 50)
    --requests      Total requests per scenario (default: 2000)
    --warmup        Warmup requests before measurement (default: 200)
    --output-dir    Directory for output files (default: tests/perf/)
"""

import argparse
import json
import os
import statistics
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Optional

import requests


# ─── Helpers ──────────────────────────────────────────────────────────────────

def make_payload(method: str, params: dict, req_id: int = 1) -> dict:
    return {"jsonrpc": "2.0", "id": req_id, "method": method, "params": params}


def do_request(url: str, payload: dict) -> tuple[float, bool]:
    """Send one POST request; return (latency_ms, success)."""
    start = time.perf_counter()
    try:
        resp = requests.post(url, json=payload, timeout=10)
        elapsed = (time.perf_counter() - start) * 1000
        ok = resp.status_code == 200 and "error" not in resp.json()
        return elapsed, ok
    except Exception:
        elapsed = (time.perf_counter() - start) * 1000
        return elapsed, False


def run_scenario(
    name: str,
    url: str,
    method: str,
    params: dict,
    concurrency: int,
    total: int,
    warmup: int,
) -> dict:
    """Run a benchmark scenario and return statistics."""
    payload = make_payload(method, params)

    print(f"  [{name}] warmup ({warmup} req)...", end=" ", flush=True)
    with ThreadPoolExecutor(max_workers=min(concurrency, warmup)) as ex:
        futures = [ex.submit(do_request, url, payload) for _ in range(warmup)]
        for f in as_completed(futures):
            f.result()
    print("done")

    print(f"  [{name}] measuring ({total} req, {concurrency} concurrent)...", end=" ", flush=True)
    latencies: list[float] = []
    errors = 0
    start_wall = time.perf_counter()

    with ThreadPoolExecutor(max_workers=concurrency) as ex:
        futures = [ex.submit(do_request, url, payload) for _ in range(total)]
        for f in as_completed(futures):
            lat, ok = f.result()
            latencies.append(lat)
            if not ok:
                errors += 1

    elapsed_wall = time.perf_counter() - start_wall
    rps = total / elapsed_wall

    latencies.sort()
    p50  = statistics.median(latencies)
    p95  = latencies[int(len(latencies) * 0.95)]
    p99  = latencies[int(len(latencies) * 0.99)]
    mean = statistics.mean(latencies)
    err_rate = errors / total * 100

    result = {
        "name":        name,
        "url":         url,
        "method":      method,
        "concurrency": concurrency,
        "total":       total,
        "rps":         round(rps, 1),
        "mean_ms":     round(mean, 2),
        "p50_ms":      round(p50, 2),
        "p95_ms":      round(p95, 2),
        "p99_ms":      round(p99, 2),
        "error_rate":  round(err_rate, 2),
        "errors":      errors,
    }
    print(f"RPS={rps:.0f}  P50={p50:.1f}ms  P95={p95:.1f}ms  P99={p99:.1f}ms  "
          f"err={err_rate:.1f}%")
    return result


# ─── Report generators ────────────────────────────────────────────────────────

def write_json(results: list[dict], path: str) -> None:
    with open(path, "w") as f:
        json.dump({"scenarios": results}, f, indent=2)
    print(f"\nJSON results → {path}")


def write_markdown(results: list[dict], path: str) -> None:
    lines = [
        "# seastar-mcp-server Performance Benchmark",
        "",
        f"Generated: {time.strftime('%Y-%m-%d %H:%M:%S')}",
        "",
        "| Scenario | RPS | Mean(ms) | P50(ms) | P95(ms) | P99(ms) | ErrorRate |",
        "|---|---|---|---|---|---|---|",
    ]
    for r in results:
        lines.append(
            f"| {r['name']} | {r['rps']} | {r['mean_ms']} "
            f"| {r['p50_ms']} | {r['p95_ms']} | {r['p99_ms']} | {r['error_rate']}% |"
        )
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"Markdown report → {path}")


# ─── Main ─────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="seastar-mcp-server benchmark")
    parser.add_argument("--host",         default="127.0.0.1")
    parser.add_argument("--sse-port",     type=int, default=8080)
    parser.add_argument("--stream-port",  type=int, default=8081)
    parser.add_argument("--concurrency",  type=int, default=50)
    parser.add_argument("--requests",     type=int, default=2000)
    parser.add_argument("--warmup",       type=int, default=200)
    parser.add_argument("--output-dir",
        default=os.path.join(os.path.dirname(__file__)))
    args = parser.parse_args()

    sse_url    = f"http://{args.host}:{args.sse_port}/message"
    stream_url = f"http://{args.host}:{args.stream_port}/mcp"

    # Verify server is reachable
    try:
        r = requests.post(sse_url,
            json=make_payload("ping", {}), timeout=3)
        r.raise_for_status()
    except Exception as e:
        print(f"ERROR: Cannot reach server at {sse_url}: {e}", file=sys.stderr)
        sys.exit(1)

    scenarios = [
        ("ping (SSE)",          sse_url,    "ping",       {}),
        ("tools/list (SSE)",    sse_url,    "tools/list", {}),
        ("tools/call sum (SSE)", sse_url,   "tools/call", {"name": "calculate_sum",
                                                            "arguments": {"a": 1, "b": 2}}),
        ("ping (Streamable)",   stream_url, "ping",       {}),
    ]

    print(f"\n{'='*60}")
    print(f"seastar-mcp-server Benchmark")
    print(f"  concurrency={args.concurrency}  requests={args.requests}  warmup={args.warmup}")
    print(f"{'='*60}\n")

    results = []
    for name, url, method, params in scenarios:
        print(f"Scenario: {name}")
        r = run_scenario(
            name, url, method, params,
            args.concurrency, args.requests, args.warmup,
        )
        results.append(r)
        print()

    os.makedirs(args.output_dir, exist_ok=True)
    write_json(results,     os.path.join(args.output_dir, "results.json"))
    write_markdown(results, os.path.join(args.output_dir, "report.md"))


if __name__ == "__main__":
    main()
