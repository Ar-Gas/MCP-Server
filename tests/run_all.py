#!/usr/bin/env python3
"""
seastar-mcp-server Master Test Runner
======================================
Runs all test suites and generates a combined report.

Steps:
  1. Build C++ unit tests (ninja)
  2. Run C++ unit tests (ctest)
  3. Start demo_server
  4. Run Python integration tests (pytest)
  5. Run performance benchmark
  6. Generate tests/report.md + tests/results.json

Usage:
    python tests/run_all.py [--skip-build] [--skip-perf] [--skip-unit]
                            [--skip-integration] [--concurrency N]
                            [--requests N] [--warmup N]
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
import datetime
import requests

_TESTS_DIR  = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT  = os.path.normpath(os.path.join(_TESTS_DIR, ".."))
_BUILD_DIR  = os.path.join(_REPO_ROOT, "build")
_SERVER_BIN = os.path.join(_BUILD_DIR, "examples", "demo", "demo_server")
_SSE_BASE   = "http://127.0.0.1:8080"


# ─── Helpers ──────────────────────────────────────────────────────────────────

def run(cmd: list[str], cwd: str = None, check: bool = True) -> subprocess.CompletedProcess:
    print(f"  $ {' '.join(cmd)}")
    return subprocess.run(cmd, cwd=cwd, check=check,
                          capture_output=True, text=True)


def section(title: str) -> None:
    print(f"\n{'='*60}")
    print(f"  {title}")
    print(f"{'='*60}")


def wait_for_server(base_url: str, timeout: float = 10.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            r = requests.post(f"{base_url}/message",
                json={"jsonrpc":"2.0","id":0,"method":"ping","params":{}},
                timeout=0.5)
            if r.status_code in (200, 202):
                return True
        except Exception:
            pass
        time.sleep(0.1)
    return False


# ─── Phase helpers ────────────────────────────────────────────────────────────

def build_unit_tests() -> dict:
    section("Phase 1: Build C++ unit tests")
    try:
        run(["ninja", "test_dispatcher", "test_registry"], cwd=_BUILD_DIR)
        print("  Build: OK")
        return {"status": "ok"}
    except subprocess.CalledProcessError as e:
        print(f"  Build FAILED:\n{e.stderr}")
        return {"status": "failed", "stderr": e.stderr}


def run_unit_tests() -> dict:
    section("Phase 2: Run C++ unit tests (ctest)")
    try:
        result = run(["ctest", "--output-on-failure",
                      "-R", "test_dispatcher|test_registry",
                      "--no-compress-output"],
                     cwd=_BUILD_DIR, check=False)
        output = result.stdout + result.stderr
        print(output[-2000:] if len(output) > 2000 else output)
        # Parse ctest summary: "100% tests passed, 0 tests failed out of 2"
        # or individual test lines: "Passed" / "Failed"
        passed = output.count(". Passed")
        failed = output.count(". Failed") + output.count("***Failed")
        # Fallback: parse "X tests failed out of Y"
        if passed == 0 and failed == 0:
            m = re.search(r"(\d+)% tests passed.*out of (\d+)", output)
            if m:
                total  = int(m.group(2))
                pct    = int(m.group(1))
                passed = round(total * pct / 100)
                failed = total - passed
            m = re.search(r"(\d+) tests? failed out of", output)
            if m: failed = int(m.group(1))
        status = "ok" if failed == 0 and result.returncode == 0 else "failed"
        print(f"  Unit tests: {passed} passed, {failed} failed")
        return {"status": status, "passed": passed, "failed": failed, "output": output}
    except Exception as e:
        return {"status": "error", "error": str(e)}


def run_integration_tests() -> dict:
    section("Phase 3: Python integration tests (pytest)")
    report_path = os.path.join(_TESTS_DIR, "results_integration.json")
    try:
        result = subprocess.run(
            [sys.executable, "-m", "pytest",
             os.path.join(_TESTS_DIR, "integration"),
             "-v", "--tb=short",
             "--json-report", f"--json-report-file={report_path}"],
            cwd=_TESTS_DIR,
            capture_output=True, text=True, check=False,
        )
        print(result.stdout[-3000:] if len(result.stdout) > 3000 else result.stdout)
        if result.stderr:
            print(result.stderr[-1000:])

        # Parse json report
        summary = {"status": "ok" if result.returncode == 0 else "failed",
                   "passed": 0, "failed": 0, "error": 0, "skipped": 0}
        if os.path.isfile(report_path):
            with open(report_path) as f:
                data = json.load(f)
            s = data.get("summary", {})
            summary.update({
                "passed":  s.get("passed", 0),
                "failed":  s.get("failed", 0),
                "error":   s.get("error",  0),
                "skipped": s.get("skipped", 0),
            })
        return summary
    except Exception as e:
        return {"status": "error", "error": str(e)}


def run_perf(concurrency: int, requests_: int, warmup: int) -> dict:
    section("Phase 4: Performance benchmark")
    bench_script = os.path.join(_TESTS_DIR, "perf", "bench.py")
    try:
        result = subprocess.run(
            [sys.executable, bench_script,
             "--concurrency", str(concurrency),
             "--requests",    str(requests_),
             "--warmup",      str(warmup)],
            cwd=_TESTS_DIR,
            capture_output=True, text=True, check=False,
        )
        print(result.stdout)
        if result.stderr:
            print(result.stderr[:500])

        perf_json = os.path.join(_TESTS_DIR, "perf", "results.json")
        if os.path.isfile(perf_json):
            with open(perf_json) as f:
                return {"status": "ok", "data": json.load(f)["scenarios"]}
        return {"status": "ok", "data": []}
    except Exception as e:
        return {"status": "error", "error": str(e)}


# ─── Report generators ────────────────────────────────────────────────────────

def generate_report(build_r: dict, unit_r: dict, integ_r: dict, perf_r: dict) -> None:
    ts  = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    try:
        git_hash = subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=_REPO_ROOT, text=True).strip()
    except Exception:
        git_hash = "unknown"

    lines = [
        "# seastar-mcp-server Test Report",
        "",
        f"Generated: {ts}  |  Build: `{git_hash}`",
        "",
        "## Unit Tests",
        "",
        "| Suite | Tests | Pass | Fail |",
        "|---|---|---|---|",
    ]

    # Unit test rows (approximate per-suite from ctest output)
    total_unit = unit_r.get("passed", 0) + unit_r.get("failed", 0)
    lines.append(
        f"| C++ (test_dispatcher + test_registry) | {total_unit} "
        f"| {unit_r.get('passed',0)} | {unit_r.get('failed',0)} |"
    )
    lines += [
        "",
        "## Integration Tests",
        "",
        "| Result | Count |",
        "|---|---|",
        f"| Passed  | {integ_r.get('passed', 0)} |",
        f"| Failed  | {integ_r.get('failed', 0)} |",
        f"| Error   | {integ_r.get('error',  0)} |",
        f"| Skipped | {integ_r.get('skipped',0)} |",
    ]

    if perf_r.get("data"):
        lines += [
            "",
            "## Performance Benchmark",
            "",
            "| Scenario | RPS | Mean(ms) | P50(ms) | P95(ms) | P99(ms) | ErrorRate |",
            "|---|---|---|---|---|---|---|",
        ]
        for s in perf_r["data"]:
            lines.append(
                f"| {s['name']} | {s['rps']} | {s['mean_ms']} "
                f"| {s['p50_ms']} | {s['p95_ms']} | {s['p99_ms']} | {s['error_rate']}% |"
            )

    report_md = os.path.join(_TESTS_DIR, "report.md")
    with open(report_md, "w") as f:
        f.write("\n".join(lines) + "\n")

    combined = {
        "generated": ts,
        "git_hash":  git_hash,
        "build":     build_r,
        "unit":      unit_r,
        "integration": integ_r,
        "perf":      perf_r,
    }
    results_json = os.path.join(_TESTS_DIR, "results.json")
    with open(results_json, "w") as f:
        json.dump(combined, f, indent=2)

    section("Summary")
    print(f"  Unit tests:        {unit_r.get('passed',0)} passed / {unit_r.get('failed',0)} failed")
    print(f"  Integration tests: {integ_r.get('passed',0)} passed / "
          f"{integ_r.get('failed',0)+integ_r.get('error',0)} failed")
    if perf_r.get("data"):
        for s in perf_r["data"]:
            print(f"  {s['name']:<30} RPS={s['rps']:<8} P99={s['p99_ms']}ms")
    print(f"\n  Report  → {report_md}")
    print(f"  Results → {results_json}")


# ─── Main ─────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="Run all seastar-mcp-server tests")
    parser.add_argument("--skip-build",       action="store_true")
    parser.add_argument("--skip-unit",        action="store_true")
    parser.add_argument("--skip-integration", action="store_true")
    parser.add_argument("--skip-perf",        action="store_true")
    parser.add_argument("--concurrency",  type=int, default=50)
    parser.add_argument("--requests",     type=int, default=2000)
    parser.add_argument("--warmup",       type=int, default=200)
    args = parser.parse_args()

    build_r = {"status": "skipped"}
    unit_r  = {"status": "skipped", "passed": 0, "failed": 0}
    integ_r = {"status": "skipped", "passed": 0, "failed": 0, "error": 0, "skipped": 0}
    perf_r  = {"status": "skipped", "data": []}

    if not args.skip_build:
        build_r = build_unit_tests()

    if not args.skip_unit:
        unit_r = run_unit_tests()

    # Start server for integration + perf tests
    server_proc = None
    if not args.skip_integration or not args.skip_perf:
        section("Starting demo_server")
        if not os.path.isfile(_SERVER_BIN):
            print(f"  WARNING: {_SERVER_BIN} not found. Skipping integration+perf tests.")
            args.skip_integration = args.skip_perf = True
        else:
            import subprocess as _sp
            server_proc = _sp.Popen(
                [_SERVER_BIN, "-c", "1", "-m", "256M",
                 "--overprovisioned", "--default-log-level=error"],
                stdout=_sp.DEVNULL, stderr=_sp.DEVNULL,
            )
            if wait_for_server(_SSE_BASE):
                print("  Server started OK")
            else:
                print("  Server failed to start. Skipping integration+perf.")
                server_proc.terminate()
                server_proc = None
                args.skip_integration = args.skip_perf = True

    try:
        if not args.skip_integration:
            integ_r = run_integration_tests()

        if not args.skip_perf:
            perf_r = run_perf(args.concurrency, args.requests, args.warmup)
    finally:
        if server_proc:
            server_proc.terminate()
            try:
                server_proc.wait(timeout=5)
            except Exception:
                server_proc.kill()

    generate_report(build_r, unit_r, integ_r, perf_r)

    # Exit with non-zero if any test failed
    failed = (unit_r.get("failed", 0) +
              integ_r.get("failed", 0) +
              integ_r.get("error", 0))
    sys.exit(1 if failed > 0 else 0)


if __name__ == "__main__":
    main()
