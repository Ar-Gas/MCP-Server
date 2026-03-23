#!/usr/bin/env python3
"""
Multi-core QPS stress test for seastar-mcp-server
====================================================

Validates Seastar's sharded architecture by:

  1. QPS SCALING  — start server with 1 / 2 / 4 cores, measure total
                    throughput and calculate scaling efficiency.

  2. CROSS-SHARD  — with 2 cores, establish SSE sessions on shard 0
                    and shard 1, then POST messages to each and compare
                    same-shard vs cross-shard routing latency.

  3. SESSION FANOUT — N concurrent SSE sessions, one message each,
                    measure total push latency under load.

Usage:
    python tests/perf/multicore_bench.py [options]

Options:
    --server-bin    Path to demo_server binary
                    (default: build/examples/demo/demo_server)
    --max-cores     Maximum core count to test  (default: 4)
    --concurrency   Concurrent client threads per test  (default: 40)
    --requests      Total requests per scenario  (default: 1000)
    --warmup        Warmup requests before measurement  (default: 100)
    --sse-port      HTTP+SSE transport port  (default: 8080)
    --stream-port   Streamable HTTP port  (default: 8081)
    --output-dir    Directory for result files  (default: tests/perf/)
    --cores-list    Explicit comma-separated list of core counts
                    (overrides --max-cores, e.g. "1,2,4")
"""

import argparse
import json
import os
import socket
import statistics
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Optional

import requests

# ── Paths ─────────────────────────────────────────────────────────────────────

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.normpath(os.path.join(_HERE, "..", ".."))
_DEFAULT_BIN = os.path.join(_REPO, "build", "examples", "demo", "demo_server")


# ── Server lifecycle ──────────────────────────────────────────────────────────

class ServerHandle:
    """Context manager that starts/stops demo_server for a given core count."""

    def __init__(self, binary: str, cores: int, sse_port: int, stream_port: int):
        self.binary     = binary
        self.cores      = cores
        self.sse_port   = sse_port
        self.stream_port = stream_port
        self._proc: Optional[subprocess.Popen] = None

    def __enter__(self) -> "ServerHandle":
        self._proc = subprocess.Popen(
            [
                self.binary,
                "-c", str(self.cores),
                "-m", "512M",
                "--overprovisioned",
                "--default-log-level=error",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if not self._wait(f"http://127.0.0.1:{self.sse_port}/message", 10.0):
            self._proc.terminate()
            self._proc.wait()
            raise RuntimeError(
                f"demo_server (cores={self.cores}) failed to start on "
                f"port {self.sse_port} within 10 s"
            )
        # Also wait for Streamable HTTP port
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            try:
                s = socket.create_connection(("127.0.0.1", self.stream_port), 0.5)
                s.close()
                break
            except OSError:
                time.sleep(0.1)
        return self

    def __exit__(self, *_):
        if self._proc:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait()

    @staticmethod
    def _wait(url: str, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        payload  = {"jsonrpc": "2.0", "id": 0, "method": "ping", "params": {}}
        while time.monotonic() < deadline:
            try:
                r = requests.post(url, json=payload, timeout=0.5)
                if r.status_code in (200, 202):
                    return True
            except Exception:
                pass
            time.sleep(0.1)
        return False


# ── Single-request driver ─────────────────────────────────────────────────────

def _post(url: str, payload: dict) -> tuple[float, bool]:
    """POST one JSON-RPC request.  Returns (latency_ms, success)."""
    t0 = time.perf_counter()
    try:
        r = requests.post(url, json=payload, timeout=10)
        ms = (time.perf_counter() - t0) * 1000
        ok = r.status_code == 200 and "error" not in r.json()
        return ms, ok
    except Exception:
        return (time.perf_counter() - t0) * 1000, False


# ── Scenario runner ───────────────────────────────────────────────────────────

def run_scenario(
    name: str,
    url: str,
    method: str,
    params: dict,
    concurrency: int,
    total: int,
    warmup: int,
) -> dict:
    payload = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params}

    # Warmup
    with ThreadPoolExecutor(max_workers=min(concurrency, warmup or 1)) as ex:
        futs = [ex.submit(_post, url, payload) for _ in range(warmup)]
        for f in as_completed(futs):
            f.result()

    # Measurement
    latencies: list[float] = []
    errors = 0
    wall_start = time.perf_counter()
    with ThreadPoolExecutor(max_workers=concurrency) as ex:
        futs = [ex.submit(_post, url, payload) for _ in range(total)]
        for f in as_completed(futs):
            ms, ok = f.result()
            latencies.append(ms)
            if not ok:
                errors += 1
    wall = time.perf_counter() - wall_start

    latencies.sort()
    n    = len(latencies)
    rps  = total / wall
    mean = statistics.mean(latencies)
    p50  = statistics.median(latencies)
    p95  = latencies[int(n * 0.95)]
    p99  = latencies[int(n * 0.99)]
    err  = errors / total * 100

    result = dict(
        name=name, url=url, method=method, concurrency=concurrency,
        total=total, rps=round(rps, 1), mean_ms=round(mean, 2),
        p50_ms=round(p50, 2), p95_ms=round(p95, 2), p99_ms=round(p99, 2),
        error_rate=round(err, 2), errors=errors,
    )
    print(f"  {name:<40}  RPS={rps:>7.0f}  P50={p50:>6.1f}ms  "
          f"P95={p95:>6.1f}ms  P99={p99:>6.1f}ms  err={err:.1f}%")
    return result


# ── Phase 1: QPS Scaling ──────────────────────────────────────────────────────

def phase_scaling(args, cores_list: list[int]) -> list[dict]:
    """
    Start the server for each core count, run ping / tools/list / tools/call,
    record RPS, then compute scaling efficiency vs the 1-core baseline.
    """
    print("\n" + "="*70)
    print("PHASE 1 — QPS Scaling  (1 → N cores)")
    print("="*70)

    sse_url    = f"http://127.0.0.1:{args.sse_port}/message"
    stream_url = f"http://127.0.0.1:{args.stream_port}/mcp"

    scenarios_def = [
        ("ping   [SSE]",        sse_url,    "ping",       {}),
        ("tools/list [SSE]",    sse_url,    "tools/list", {}),
        ("tools/call [SSE]",    sse_url,    "tools/call",
         {"name": "calculate_sum", "arguments": {"a": 3, "b": 4}}),
        ("ping   [Streamable]", stream_url, "ping",       {}),
    ]

    # baseline_rps[method] from 1-core run
    baseline: dict[str, float] = {}
    all_results: list[dict] = []

    for c in sorted(cores_list):
        print(f"\n── {c} core(s) ──")
        with ServerHandle(args.server_bin, c, args.sse_port, args.stream_port):
            # scale concurrency with core count to keep each core equally loaded
            conc = min(args.concurrency * c, 200)
            for sname, url, method, params in scenarios_def:
                r = run_scenario(
                    f"cores={c}  {sname}", url, method, params,
                    conc, args.requests, args.warmup,
                )
                # attach scaling metadata
                r["cores"] = c
                if c == 1:
                    baseline[method] = r["rps"]
                    r["scaling_efficiency"] = 1.0
                else:
                    base = baseline.get(method, r["rps"])
                    r["scaling_efficiency"] = round(
                        r["rps"] / (c * base) if base else 0, 3
                    )
                all_results.append(r)

    # Print scaling summary
    print("\n── Scaling efficiency summary (ideal = 1.00) ──")
    print(f"  {'Scenario':<36}  {'1c RPS':>8}  {'2c RPS':>8}  {'4c RPS':>8}  "
          f"{'2c eff':>7}  {'4c eff':>7}")
    by_method: dict[str, dict] = {}
    for r in all_results:
        by_method.setdefault(r["method"], {})[r["cores"]] = r
    for meth, cmap in by_method.items():
        r1 = cmap.get(1, {})
        r2 = cmap.get(2, {})
        r4 = cmap.get(4, {})
        print(f"  {meth:<36}  "
              f"{r1.get('rps','-'):>8}  "
              f"{r2.get('rps','-'):>8}  "
              f"{r4.get('rps','-'):>8}  "
              f"{r2.get('scaling_efficiency','-'):>7}  "
              f"{r4.get('scaling_efficiency','-'):>7}")

    return all_results


# ── Phase 2: Cross-shard SSE routing ─────────────────────────────────────────

def _get_session_id(sse_base: str) -> Optional[str]:
    """Open GET /sse for 0.5s, parse the sessionId from the endpoint event."""
    try:
        r = requests.get(f"{sse_base}/sse",
                         headers={"Accept": "text/event-stream"},
                         stream=True, timeout=3)
        for line in r.iter_lines(decode_unicode=True):
            if "sessionId=" in line:
                return line.split("sessionId=")[-1].strip().split()[0]
    except Exception:
        pass
    return None


def _shard_of(session_id: str) -> int:
    """Extract shard ID from session_id: 's{N}_{counter}' → N."""
    if not session_id.startswith("s"):
        return 0
    idx = 2 if session_id.startswith("sm") else 1
    pos = session_id.find("_", idx)
    if pos == -1:
        return 0
    try:
        return int(session_id[idx:pos])
    except ValueError:
        return 0


def _post_to_session(sse_url: str, session_id: str) -> tuple[float, bool]:
    """POST a ping to a specific SSE session, return (ms, ok)."""
    url = f"{sse_url}?sessionId={session_id}"
    t0  = time.perf_counter()
    try:
        r = requests.post(url,
                          json={"jsonrpc": "2.0", "id": 1, "method": "ping", "params": {}},
                          timeout=5)
        ms = (time.perf_counter() - t0) * 1000
        return ms, r.status_code in (200, 202)
    except Exception:
        return (time.perf_counter() - t0) * 1000, False


def phase_cross_shard(args) -> list[dict]:
    """
    Run with 2 cores.  Establish sessions on shard 0 and shard 1 by polling
    GET /sse until we have at least SESSIONS_PER_SHARD for each.  Then
    concurrently POST messages to each group and compare latencies.
    """
    SESSIONS_PER_SHARD = 10
    MSGS_PER_SESSION   = 20

    print("\n" + "="*70)
    print("PHASE 2 — Cross-shard SSE routing  (2 cores)")
    print("="*70)

    sse_base   = f"http://127.0.0.1:{args.sse_port}"
    message_url = f"{sse_base}/message"
    results = []

    with ServerHandle(args.server_bin, 2, args.sse_port, args.stream_port):
        # Collect session IDs grouped by shard
        by_shard: dict[int, list[str]] = {0: [], 1: []}
        print(f"\n  Establishing {SESSIONS_PER_SHARD} sessions per shard...", end=" ", flush=True)
        attempts = 0
        while (len(by_shard[0]) < SESSIONS_PER_SHARD or
               len(by_shard[1]) < SESSIONS_PER_SHARD):
            sid = _get_session_id(sse_base)
            if sid:
                sh = _shard_of(sid)
                if sh in by_shard and len(by_shard[sh]) < SESSIONS_PER_SHARD:
                    by_shard[sh].append(sid)
            attempts += 1
            if attempts > 200:
                print(f"\n  WARNING: only got shard0={len(by_shard[0])}, "
                      f"shard1={len(by_shard[1])} after {attempts} attempts.")
                break
        print(f"done  (shard0={len(by_shard[0])}, shard1={len(by_shard[1])})")

        for label, sessions in [("shard-0 sessions", by_shard[0]),
                                 ("shard-1 sessions", by_shard[1])]:
            if not sessions:
                continue
            # Send MSGS_PER_SESSION messages to each session concurrently
            tasks = [(message_url, sid)
                     for sid in sessions
                     for _ in range(MSGS_PER_SESSION)]
            latencies = []
            errors = 0

            # Warmup
            warmup_tasks = tasks[:min(20, len(tasks))]
            with ThreadPoolExecutor(max_workers=args.concurrency) as ex:
                futs = [ex.submit(_post_to_session, u, s) for u, s in warmup_tasks]
                for f in as_completed(futs):
                    f.result()

            wall_start = time.perf_counter()
            with ThreadPoolExecutor(max_workers=args.concurrency) as ex:
                futs = [ex.submit(_post_to_session, u, s) for u, s in tasks]
                for f in as_completed(futs):
                    ms, ok = f.result()
                    latencies.append(ms)
                    if not ok:
                        errors += 1
            wall = time.perf_counter() - wall_start

            latencies.sort()
            n   = len(latencies)
            rps = n / wall
            p50 = statistics.median(latencies)
            p95 = latencies[int(n * 0.95)]
            p99 = latencies[int(n * 0.99)]
            err = errors / n * 100

            r = dict(
                name=f"cross-shard: POST→{label}",
                url=message_url, method="ping (SSE session)",
                concurrency=args.concurrency, total=n,
                rps=round(rps, 1), mean_ms=round(statistics.mean(latencies), 2),
                p50_ms=round(p50, 2), p95_ms=round(p95, 2), p99_ms=round(p99, 2),
                error_rate=round(err, 2), errors=errors, cores=2,
                scaling_efficiency=None,
            )
            results.append(r)
            print(f"  POST→{label:<22}  RPS={rps:>7.0f}  P50={p50:>6.1f}ms  "
                  f"P95={p95:>6.1f}ms  P99={p99:>6.1f}ms  err={err:.1f}%")

        # Routing overhead: compare shard0 vs shard1 median latency
        if len(results) == 2:
            delta = abs(results[0]["p50_ms"] - results[1]["p50_ms"])
            print(f"\n  Cross-shard routing overhead (P50 delta): {delta:.2f} ms")

    return results


# ── Phase 3: Concurrent SSE session fanout ────────────────────────────────────

def phase_session_fanout(args) -> list[dict]:
    """
    Establish N concurrent SSE sessions (2-core server), push one message
    to each simultaneously, measure total push throughput and latency.
    """
    SESSION_COUNTS = [10, 50, 100]

    print("\n" + "="*70)
    print("PHASE 3 — Concurrent SSE session fanout  (2 cores)")
    print("="*70)

    sse_base    = f"http://127.0.0.1:{args.sse_port}"
    message_url = f"{sse_base}/message"
    results = []

    with ServerHandle(args.server_bin, 2, args.sse_port, args.stream_port):
        for n_sessions in SESSION_COUNTS:
            # Gather n_sessions session IDs
            sessions: list[str] = []
            attempts = 0
            while len(sessions) < n_sessions and attempts < n_sessions * 5:
                sid = _get_session_id(sse_base)
                if sid:
                    sessions.append(sid)
                attempts += 1

            if len(sessions) < n_sessions:
                print(f"  WARNING: only got {len(sessions)}/{n_sessions} sessions")

            if not sessions:
                continue

            tasks = [(message_url, sid) for sid in sessions]
            latencies = []
            errors = 0

            wall_start = time.perf_counter()
            with ThreadPoolExecutor(max_workers=len(sessions)) as ex:
                futs = [ex.submit(_post_to_session, u, s) for u, s in tasks]
                for f in as_completed(futs):
                    ms, ok = f.result()
                    latencies.append(ms)
                    if not ok:
                        errors += 1
            wall = time.perf_counter() - wall_start

            latencies.sort()
            n   = len(latencies)
            rps = n / wall
            p50 = statistics.median(latencies)
            p99 = latencies[int(n * 0.99)]
            err = errors / n * 100

            r = dict(
                name=f"session-fanout n={n_sessions}",
                url=message_url, method="ping (fanout)",
                concurrency=n_sessions, total=n,
                rps=round(rps, 1), mean_ms=round(statistics.mean(latencies), 2),
                p50_ms=round(p50, 2), p95_ms=round(p50, 2),
                p99_ms=round(p99, 2),
                error_rate=round(err, 2), errors=errors, cores=2,
                scaling_efficiency=None,
            )
            results.append(r)
            print(f"  {n_sessions:>3} sessions, 1 msg each: "
                  f"RPS={rps:>7.0f}  P50={p50:>6.1f}ms  P99={p99:>6.1f}ms  err={err:.1f}%")

    return results


# ── Report writers ────────────────────────────────────────────────────────────

def write_json(all_results: list[dict], path: str) -> None:
    with open(path, "w") as f:
        json.dump({"generated": time.strftime("%Y-%m-%d %H:%M:%S"),
                   "scenarios": all_results}, f, indent=2)
    print(f"\nJSON  → {path}")


def write_markdown(all_results: list[dict], path: str) -> None:
    lines = [
        "# seastar-mcp-server Multi-core Benchmark",
        "",
        f"Generated: {time.strftime('%Y-%m-%d %H:%M:%S')}",
        "",
        "## QPS Scaling (cores × throughput)",
        "",
        "| Scenario | Cores | RPS | Mean(ms) | P50(ms) | P95(ms) | P99(ms) | Eff | Err% |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for r in all_results:
        eff = f"{r['scaling_efficiency']:.3f}" if r.get("scaling_efficiency") is not None else "—"
        lines.append(
            f"| {r['name']} | {r.get('cores','—')} | {r['rps']} | {r['mean_ms']} "
            f"| {r['p50_ms']} | {r['p95_ms']} | {r['p99_ms']} | {eff} | {r['error_rate']}% |"
        )
    lines += [
        "",
        "> **Eff** = actual_RPS / (N_cores × single-core_RPS). Ideal = 1.00.",
        "",
    ]
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"Markdown → {path}")


# ── main ──────────────────────────────────────────────────────────────────────

def main() -> None:
    p = argparse.ArgumentParser(description="Multi-core QPS stress test")
    p.add_argument("--server-bin",   default=_DEFAULT_BIN)
    p.add_argument("--max-cores",    type=int, default=4)
    p.add_argument("--cores-list",   default="",
                   help="Comma-separated core counts, e.g. 1,2,4")
    p.add_argument("--concurrency",  type=int, default=40)
    p.add_argument("--requests",     type=int, default=1000)
    p.add_argument("--warmup",       type=int, default=100)
    p.add_argument("--sse-port",     type=int, default=8080)
    p.add_argument("--stream-port",  type=int, default=8081)
    p.add_argument("--output-dir",   default=_HERE)
    p.add_argument("--skip-cross-shard", action="store_true")
    p.add_argument("--skip-fanout",      action="store_true")
    args = p.parse_args()

    if not os.path.isfile(args.server_bin):
        print(f"ERROR: server binary not found: {args.server_bin}", file=sys.stderr)
        sys.exit(1)

    # Build cores list
    if args.cores_list:
        cores_list = [int(x) for x in args.cores_list.split(",")]
    else:
        import multiprocessing
        avail = multiprocessing.cpu_count()
        cores_list = [c for c in [1, 2, 4] if c <= min(args.max_cores, avail)]
        if not cores_list:
            cores_list = [1]

    print(f"\n{'='*70}")
    print(f"seastar-mcp-server Multi-core Benchmark")
    print(f"  server:      {args.server_bin}")
    print(f"  cores:       {cores_list}")
    print(f"  concurrency: {args.concurrency} per core")
    print(f"  requests:    {args.requests}  warmup: {args.warmup}")
    print(f"{'='*70}")

    all_results: list[dict] = []

    # Phase 1: QPS scaling
    all_results.extend(phase_scaling(args, cores_list))

    # Phase 2: cross-shard routing (only if 2+ cores available)
    if not args.skip_cross_shard and max(cores_list) >= 2:
        try:
            all_results.extend(phase_cross_shard(args))
        except Exception as e:
            print(f"\n  Phase 2 skipped: {e}")

    # Phase 3: session fanout (only if 2+ cores available)
    if not args.skip_fanout and max(cores_list) >= 2:
        try:
            all_results.extend(phase_session_fanout(args))
        except Exception as e:
            print(f"\n  Phase 3 skipped: {e}")

    # Write reports
    os.makedirs(args.output_dir, exist_ok=True)
    write_json(all_results,     os.path.join(args.output_dir, "multicore_results.json"))
    write_markdown(all_results, os.path.join(args.output_dir, "multicore_report.md"))
    print("\nDone.")


if __name__ == "__main__":
    main()
