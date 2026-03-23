"""
Integration tests for HTTP+SSE transport (port 8080).
"""
import subprocess
import pytest
import requests
from conftest import SSE_BASE, post_message


def test_sse_port_accessible(server):
    """GET /sse on port 8080 is accepted (connection established)."""
    import socket
    s = socket.create_connection(("127.0.0.1", 8080), timeout=2)
    s.close()


def test_sse_session_established(server):
    """GET /sse streams the endpoint event containing a sessionId."""
    result = subprocess.run(
        ["curl", "-s", "--max-time", "3",
         "-H", "Accept: text/event-stream",
         f"{SSE_BASE}/sse"],
        capture_output=True, text=True, timeout=5,
    )
    output = result.stdout
    assert "sessionId" in output, \
        f"No sessionId in SSE output: {repr(output[:300])}"
    assert "event: endpoint" in output


def test_message_via_session_id(server):
    """Obtain an SSE sessionId, then route a JSON-RPC message through it."""
    result = subprocess.run(
        ["curl", "-s", "--max-time", "3",
         "-H", "Accept: text/event-stream",
         f"{SSE_BASE}/sse"],
        capture_output=True, text=True, timeout=5,
    )
    output = result.stdout
    assert "sessionId" in output, f"No sessionId in SSE stream: {repr(output[:300])}"
    session_id = output.split("sessionId=")[-1].strip().split()[0]

    # POST a ping; server pushes response to SSE queue (returns 202)
    resp = requests.post(
        f"{SSE_BASE}/message?sessionId={session_id}",
        json={"jsonrpc": "2.0", "id": 99, "method": "ping", "params": {}},
        timeout=5,
    )
    assert resp.status_code in (200, 202)
