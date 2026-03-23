#include <seastar/testing/test_case.hh>
#include <mcp/core/registry.hh>
#include <mcp/core/interfaces.hh>
#include <mcp/protocol/json_rpc.hh>
#include <nlohmann/json.hpp>
#include <memory>
#include <string>

using json = nlohmann::json;
using namespace mcp::core;

// ─── Mock 实现 ────────────────────────────────────────────────────────────────

struct SimpleTool : public McpTool {
    std::string _name;
    std::string _title;
    std::string _result_text;

    SimpleTool(std::string name, std::string title, std::string result_text = "ok")
        : _name(std::move(name)), _title(std::move(title)), _result_text(std::move(result_text)) {}

    std::string get_name()  const override { return _name; }
    std::string get_title() const override { return _title; }

    json get_definition() const override {
        return {{"name", _name}, {"inputSchema", {{"type", "object"}}}};
    }

    seastar::future<json> execute(const json&) override {
        json result;
        result["content"] = json::array({{{"type", "text"}, {"text", _result_text}}});
        co_return result;
    }
};

struct ToolWithMeta : public SimpleTool {
    ToolWithMeta() : SimpleTool("meta_tool", "Meta Tool") {}

    std::optional<json> get_output_schema() const override {
        return json{{"type","object"},{"properties",{{"value",{{"type","number"}}}}}};
    }
    json get_annotations() const override {
        return {{"readOnlyHint", true}};
    }
    std::optional<std::string> get_icon_uri() const override {
        return "https://example.com/icon.png";
    }
};

struct SimpleResource : public McpResource {
    std::string get_uri()  const override { return "test://info"; }
    std::string get_name() const override { return "test_resource"; }
    json get_definition() const override {
        return {{"uri", get_uri()}, {"name", get_name()}, {"mimeType", "text/plain"}};
    }
    seastar::future<std::string> read() override {
        co_return "hello resource";
    }
};

struct SimplePrompt : public McpPrompt {
    std::string get_name() const override { return "test_prompt"; }
    json get_definition() const override {
        return {{"name", get_name()}, {"description", "test prompt desc"}};
    }
    seastar::future<json> get_messages(const json&) override {
        json msgs = json::array();
        msgs.push_back({{"role","user"},{"content",{{"type","text"},{"text","hello"}}}});
        co_return msgs;
    }
};

// ─── Tests ───────────────────────────────────────────────────────────────────

SEASTAR_TEST_CASE(test_register_and_list_tool) {
    McpRegistry reg;
    reg.register_tool(std::make_shared<SimpleTool>("my_tool", "My Tool"));
    auto list = reg.get_tools_list();
    BOOST_REQUIRE(list.is_array());
    BOOST_REQUIRE_EQUAL(list.size(), 1u);
    BOOST_REQUIRE_EQUAL(list[0]["name"].get<std::string>(), "my_tool");
    BOOST_REQUIRE(list[0].contains("title"));
    BOOST_REQUIRE_EQUAL(list[0]["title"].get<std::string>(), "My Tool");
    co_return;
}

SEASTAR_TEST_CASE(test_auto_title_injection) {
    // definition 不含 title → registry 自动注入 get_title()
    McpRegistry reg;
    auto tool = std::make_shared<SimpleTool>("no_title_tool", "Auto Injected Title");
    reg.register_tool(tool);
    auto list = reg.get_tools_list();
    BOOST_REQUIRE(list[0].contains("title"));
    BOOST_REQUIRE_EQUAL(list[0]["title"].get<std::string>(), "Auto Injected Title");
    co_return;
}

SEASTAR_TEST_CASE(test_output_schema_injection) {
    McpRegistry reg;
    reg.register_tool(std::make_shared<ToolWithMeta>());
    auto list = reg.get_tools_list();
    BOOST_REQUIRE(list[0].contains("outputSchema"));
    BOOST_REQUIRE_EQUAL(list[0]["outputSchema"]["type"].get<std::string>(), "object");
    co_return;
}

SEASTAR_TEST_CASE(test_annotations_injection) {
    McpRegistry reg;
    reg.register_tool(std::make_shared<ToolWithMeta>());
    auto list = reg.get_tools_list();
    BOOST_REQUIRE(list[0].contains("annotations"));
    BOOST_REQUIRE_EQUAL(list[0]["annotations"]["readOnlyHint"].get<bool>(), true);
    co_return;
}

SEASTAR_TEST_CASE(test_tool_not_found) {
    McpRegistry reg;
    bool threw = false;
    try {
        co_await reg.call_tool("nonexistent", json::object());
    } catch (const mcp::protocol::JsonRpcException&) {
        threw = true;
    }
    BOOST_REQUIRE(threw);
    co_return;
}

SEASTAR_TEST_CASE(test_call_tool_success) {
    McpRegistry reg;
    reg.register_tool(std::make_shared<SimpleTool>("echo_tool", "Echo", "echo result"));
    auto result = co_await reg.call_tool("echo_tool", json::object());
    BOOST_REQUIRE(result.contains("isError"));
    BOOST_REQUIRE_EQUAL(result["isError"].get<bool>(), false);
    BOOST_REQUIRE(result.contains("content"));
    co_return;
}

SEASTAR_TEST_CASE(test_pagination_tools_list) {
    McpRegistry reg;
    // 注册 55 个工具（超过 page_size=50）
    for (int i = 0; i < 55; ++i) {
        reg.register_tool(std::make_shared<SimpleTool>(
            "tool_" + std::to_string(i), "Tool " + std::to_string(i)));
    }
    auto list = reg.get_tools_list();
    // 无分页参数时 get_tools_list() 返回全部，实际分页由 mcp_server.cc 的 tools/list handler 处理
    BOOST_REQUIRE_EQUAL(list.size(), 55u);
    co_return;
}

SEASTAR_TEST_CASE(test_register_and_read_resource) {
    McpRegistry reg;
    reg.register_resource(std::make_shared<SimpleResource>());
    auto result = co_await reg.read_resource("test://info");
    BOOST_REQUIRE(result.contains("contents"));
    BOOST_REQUIRE(result["contents"].is_array());
    BOOST_REQUIRE(!result["contents"].empty());
    auto& content = result["contents"][0];
    BOOST_REQUIRE_EQUAL(content["uri"].get<std::string>(), "test://info");
    BOOST_REQUIRE_EQUAL(content["text"].get<std::string>(), "hello resource");
    co_return;
}

SEASTAR_TEST_CASE(test_resource_not_found) {
    McpRegistry reg;
    bool threw = false;
    try {
        co_await reg.read_resource("test://nonexistent");
    } catch (const mcp::protocol::JsonRpcException&) {
        threw = true;
    }
    BOOST_REQUIRE(threw);
    co_return;
}

SEASTAR_TEST_CASE(test_prompt_get) {
    McpRegistry reg;
    reg.register_prompt(std::make_shared<SimplePrompt>());
    auto result = co_await reg.get_prompt("test_prompt", json::object());
    BOOST_REQUIRE(result.contains("messages"));
    BOOST_REQUIRE(result["messages"].is_array());
    BOOST_REQUIRE(!result["messages"].empty());
    BOOST_REQUIRE(result.contains("description"));
    BOOST_REQUIRE_EQUAL(result["description"].get<std::string>(), "test prompt desc");
    co_return;
}
