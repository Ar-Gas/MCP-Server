"""
Integration tests for MCP protocol methods via HTTP+SSE transport.
"""
import pytest
from conftest import post_message, SSE_BASE


# ─── initialize ─────────────────────────────────────────────────────────────

def test_initialize(server):
    resp = post_message("initialize", {
        "protocolVersion": "2025-11-25",
        "capabilities": {},
        "clientInfo": {"name": "pytest", "version": "1.0"},
    })
    assert "result" in resp, f"unexpected response: {resp}"
    result = resp["result"]
    assert result["protocolVersion"] == "2025-11-25", \
        f"expected 2025-11-25, got {result.get('protocolVersion')}"
    caps = result.get("capabilities", {})
    assert "resources" in caps
    assert caps["resources"].get("subscribe") is True


# ─── ping ────────────────────────────────────────────────────────────────────

def test_ping(server):
    resp = post_message("ping", {})
    assert "result" in resp
    # result must be {} (empty object or null)
    assert resp["result"] == {} or resp["result"] is None


# ─── tools/list ─────────────────────────────────────────────────────────────

def test_tools_list(server):
    resp = post_message("tools/list", {})
    assert "result" in resp
    tools = resp["result"]["tools"]
    assert isinstance(tools, list)
    assert len(tools) > 0
    for t in tools:
        assert "name" in t
        assert "title" in t
        assert "inputSchema" in t


def test_tools_list_pagination(server):
    """Follow nextCursor until exhausted."""
    params = {}
    all_tools = []
    while True:
        resp = post_message("tools/list", params)
        tools = resp["result"]["tools"]
        all_tools.extend(tools)
        cursor = resp["result"].get("nextCursor")
        if not cursor:
            break
        params = {"cursor": cursor}
    assert len(all_tools) > 0


# ─── tools/call ──────────────────────────────────────────────────────────────

def test_tools_call_calculate_sum(server):
    resp = post_message("tools/call", {"name": "calculate_sum", "arguments": {"a": 3, "b": 4}})
    assert "result" in resp
    result = resp["result"]
    assert result.get("isError") is False
    content = result["content"]
    assert any("7" in item["text"] for item in content if item.get("type") == "text")


def test_tools_call_get_current_time(server):
    resp = post_message("tools/call", {"name": "get_current_time", "arguments": {}})
    assert "result" in resp
    result = resp["result"]
    assert result.get("isError") is False
    content = result["content"]
    assert len(content) > 0
    assert content[0].get("type") == "text"
    assert len(content[0]["text"]) > 0


def test_tools_call_structured_output(server):
    """calculate_sum result always carries isError: false."""
    resp = post_message("tools/call", {"name": "calculate_sum", "arguments": {"a": 0, "b": 0}})
    assert resp["result"]["isError"] is False


def test_tools_call_invalid_tool(server):
    resp = post_message("tools/call", {"name": "nonexistent_tool_xyz", "arguments": {}})
    # Server should return a JSON-RPC error (not crash)
    assert "error" in resp or resp.get("result", {}).get("isError") is True


# ─── resources/list ──────────────────────────────────────────────────────────

def test_resources_list(server):
    resp = post_message("resources/list", {})
    assert "result" in resp
    resources = resp["result"]["resources"]
    assert isinstance(resources, list)
    for r in resources:
        assert "uri" in r


# ─── resources/read ──────────────────────────────────────────────────────────

def test_resources_read(server):
    resp = post_message("resources/read", {"uri": "sys://memory-info"})
    assert "result" in resp, f"unexpected: {resp}"
    contents = resp["result"]["contents"]
    assert len(contents) > 0
    assert "text" in contents[0]
    assert len(contents[0]["text"]) > 0


# ─── resources/subscribe ─────────────────────────────────────────────────────

def test_resources_subscribe(server):
    resp = post_message("resources/subscribe", {"uri": "sys://memory-info"})
    assert "result" in resp
    assert resp["result"] == {} or resp["result"] is None


# ─── prompts/list ────────────────────────────────────────────────────────────

def test_prompts_list(server):
    resp = post_message("prompts/list", {})
    assert "result" in resp
    prompts = resp["result"]["prompts"]
    assert isinstance(prompts, list)
    assert len(prompts) > 0
    for p in prompts:
        assert "name" in p


# ─── prompts/get ─────────────────────────────────────────────────────────────

def test_prompts_get(server):
    resp = post_message("prompts/get", {"name": "analyze_server_health", "arguments": {}})
    assert "result" in resp, f"unexpected: {resp}"
    result = resp["result"]
    assert "messages" in result
    assert isinstance(result["messages"], list)
    assert len(result["messages"]) > 0
    assert "description" in result


# ─── logging/setLevel ────────────────────────────────────────────────────────

def test_logging_setLevel(server):
    resp = post_message("logging/setLevel", {"level": "info"})
    assert "result" in resp
    # restore
    post_message("logging/setLevel", {"level": "error"})


# ─── roots/list ──────────────────────────────────────────────────────────────

def test_roots_list(server):
    resp = post_message("roots/list", {})
    assert "result" in resp
    assert "roots" in resp["result"]
    assert isinstance(resp["result"]["roots"], list)
