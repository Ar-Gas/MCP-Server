#include <seastar/testing/test_case.hh>
#include <mcp/router/dispatcher.hh>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace mcp::router;

// ─── test_single_request ────────────────────────────────────────────────────
SEASTAR_TEST_CASE(test_single_request) {
    JsonRpcDispatcher dispatcher;
    dispatcher.register_method("ping", [](const json&) -> seastar::future<json> {
        co_return json{{"pong", true}};
    });
    auto result = co_await dispatcher.handle_request(
        R"({"jsonrpc":"2.0","id":1,"method":"ping","params":{}})");
    BOOST_REQUIRE(result.has_value());
    auto j = json::parse(*result);
    BOOST_REQUIRE_EQUAL(j["id"].get<int>(), 1);
    BOOST_REQUIRE(j.contains("result"));
    BOOST_REQUIRE_EQUAL(j["result"]["pong"].get<bool>(), true);
    co_return;
}

// ─── test_method_not_found ───────────────────────────────────────────────────
SEASTAR_TEST_CASE(test_method_not_found) {
    JsonRpcDispatcher dispatcher;
    auto result = co_await dispatcher.handle_request(
        R"({"jsonrpc":"2.0","id":2,"method":"no_such_method","params":{}})");
    BOOST_REQUIRE(result.has_value());
    auto j = json::parse(*result);
    BOOST_REQUIRE(j.contains("error"));
    BOOST_REQUIRE_EQUAL(j["error"]["code"].get<int>(), -32601);
    co_return;
}

// ─── test_batch_request ──────────────────────────────────────────────────────
SEASTAR_TEST_CASE(test_batch_request) {
    JsonRpcDispatcher dispatcher;
    dispatcher.register_method("add", [](const json& p) -> seastar::future<json> {
        co_return json{{"sum", p.value("a", 0) + p.value("b", 0)}};
    });
    auto result = co_await dispatcher.handle_request(
        R"([
            {"jsonrpc":"2.0","id":1,"method":"add","params":{"a":3,"b":4}},
            {"jsonrpc":"2.0","id":2,"method":"add","params":{"a":10,"b":20}}
        ])");
    BOOST_REQUIRE(result.has_value());
    auto j = json::parse(*result);
    BOOST_REQUIRE(j.is_array());
    BOOST_REQUIRE_EQUAL(j.size(), 2u);
    // 响应顺序与请求顺序一致
    bool found7 = false, found30 = false;
    for (const auto& resp : j) {
        if (resp["result"]["sum"].get<int>() == 7)  found7  = true;
        if (resp["result"]["sum"].get<int>() == 30) found30 = true;
    }
    BOOST_REQUIRE(found7 && found30);
    co_return;
}

// ─── test_notification_no_response ──────────────────────────────────────────
SEASTAR_TEST_CASE(test_notification_no_response) {
    JsonRpcDispatcher dispatcher;
    bool called = false;
    dispatcher.register_notification("notify/test", [&called](const json&) -> seastar::future<> {
        called = true;
        return seastar::make_ready_future<>();
    });
    auto result = co_await dispatcher.handle_request(
        R"({"jsonrpc":"2.0","method":"notify/test","params":{}})");
    // 通知没有 id，不应返回响应
    BOOST_REQUIRE(!result.has_value());
    co_return;
}

// ─── test_invalid_json ───────────────────────────────────────────────────────
SEASTAR_TEST_CASE(test_invalid_json) {
    JsonRpcDispatcher dispatcher;
    auto result = co_await dispatcher.handle_request("not-json{{{");
    BOOST_REQUIRE(result.has_value());
    auto j = json::parse(*result);
    BOOST_REQUIRE(j.contains("error"));
    BOOST_REQUIRE_EQUAL(j["error"]["code"].get<int>(), -32700);
    co_return;
}

// ─── test_invalid_jsonrpc ────────────────────────────────────────────────────
SEASTAR_TEST_CASE(test_invalid_jsonrpc) {
    JsonRpcDispatcher dispatcher;
    // 缺少 jsonrpc 字段
    auto result = co_await dispatcher.handle_request(
        R"({"id":1,"method":"ping","params":{}})");
    BOOST_REQUIRE(result.has_value());
    auto j = json::parse(*result);
    BOOST_REQUIRE(j.contains("error"));
    BOOST_REQUIRE_EQUAL(j["error"]["code"].get<int>(), -32600);
    co_return;
}

// ─── test_batch_empty_array ──────────────────────────────────────────────────
SEASTAR_TEST_CASE(test_batch_empty_array) {
    JsonRpcDispatcher dispatcher;
    auto result = co_await dispatcher.handle_request("[]");
    BOOST_REQUIRE(result.has_value());
    auto j = json::parse(*result);
    BOOST_REQUIRE(j.contains("error"));
    BOOST_REQUIRE_EQUAL(j["error"]["code"].get<int>(), -32600);
    co_return;
}
