"""
pytest fixtures: start/stop the demo_server process for integration tests.
"""
import os
import subprocess
import time
import pytest
import requests

# Paths and URLs
_TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT  = os.path.normpath(os.path.join(_TESTS_DIR, "..", ".."))
SERVER_BIN  = os.path.join(_REPO_ROOT, "build", "examples", "demo", "demo_server")
SSE_BASE    = "http://127.0.0.1:8080"
STREAM_BASE = "http://127.0.0.1:8081"


def _wait_for_server(base_url: str, timeout: float = 10.0) -> bool:
    """Poll until the server responds or timeout expires."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            resp = requests.post(
                f"{base_url}/message",
                json={"jsonrpc": "2.0", "id": 0, "method": "ping", "params": {}},
                timeout=0.5,
            )
            if resp.status_code in (200, 202):
                return True
        except Exception:
            pass
        time.sleep(0.1)
    return False


@pytest.fixture(scope="session")
def server():
    """Start demo_server once for the whole test session."""
    if not os.path.isfile(SERVER_BIN):
        pytest.skip(f"demo_server not found at {SERVER_BIN}. Run 'cd build && ninja' first.")

    proc = subprocess.Popen(
        [
            SERVER_BIN,
            "-c", "1",
            "-m", "256M",
            "--overprovisioned",
            "--default-log-level=error",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    if not _wait_for_server(SSE_BASE):
        proc.terminate()
        proc.wait()
        pytest.fail("demo_server failed to start within 10 seconds")

    # Also wait for the Streamable HTTP port (uses /mcp not /message)
    import socket as _sock
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        try:
            s = _sock.create_connection(("127.0.0.1", 8081), timeout=0.5)
            s.close()
            break
        except OSError:
            time.sleep(0.1)

    yield proc

    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()


def post_message(method: str, params: dict = None, req_id: int = 1,
                 base_url: str = SSE_BASE, session_id: str = None) -> dict:
    """Helper: send a JSON-RPC request via POST /message."""
    url = f"{base_url}/message"
    if session_id:
        url += f"?sessionId={session_id}"
    payload = {"jsonrpc": "2.0", "id": req_id, "method": method,
               "params": params or {}}
    resp = requests.post(url, json=payload, timeout=10)
    resp.raise_for_status()
    return resp.json()
