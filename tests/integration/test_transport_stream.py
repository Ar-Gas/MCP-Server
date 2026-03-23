"""
Integration tests for Streamable HTTP transport (port 8081, POST /mcp).
"""
import subprocess
import requests
import pytest
from conftest import STREAM_BASE


MCP_URL = f"{STREAM_BASE}/mcp"


def _mcp_post(method: str, params: dict = None, req_id: int = 1,
              accept: str = "application/json") -> requests.Response:
    payload = {"jsonrpc": "2.0", "id": req_id, "method": method,
               "params": params or {}}
    headers = {"Content-Type": "application/json", "Accept": accept}
    return requests.post(MCP_URL, json=payload, headers=headers, timeout=10)


def test_streamable_http_ping(server):
    """POST /mcp with application/json → synchronous JSON response."""
    resp = _mcp_post("ping", {})
    assert resp.status_code == 200
    data = resp.json()
    assert "result" in data or "error" in data


def test_streamable_http_tools_list(server):
    """POST /mcp tools/list → tools array non-empty."""
    resp = _mcp_post("tools/list", {})
    assert resp.status_code == 200
    data = resp.json()
    assert "result" in data
    tools = data["result"]["tools"]
    assert isinstance(tools, list)
    assert len(tools) > 0


def test_streamable_http_sse_mode(server):
    """POST /mcp with Accept: text/event-stream → SSE content-type response."""
    result = subprocess.run(
        ["curl", "-si", "--max-time", "3",
         "-X", "POST",
         "-H", "Content-Type: application/json",
         "-H", "Accept: text/event-stream",
         "-d", '{"jsonrpc":"2.0","id":1,"method":"ping","params":{}}',
         MCP_URL],
        capture_output=True, text=True, timeout=5,
    )
    output = result.stdout
    assert "text/event-stream" in output, \
        f"Expected SSE content-type. curl output: {repr(output[:300])}"
