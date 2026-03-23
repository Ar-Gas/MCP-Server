#include "mcp/server/mcp_server.hh"
#include "mcp/server/mcp_shard.hh"
#include "mcp/transport/stdio_transport.hh"
#include "mcp/transport/http_sse_transport.hh"
#include "mcp/transport/streamable_http_transport.hh"
#include <seastar/core/coroutine.hh>
#include <seastar/util/log.hh>

using json = nlohmann::json;

namespace mcp::server {

static seastar::logger server_log("mcp_server");

// ── McpShard ─────────────────────────────────────────────────────────────────

McpShard::McpShard(McpServerConfig config, std::shared_ptr<mcp::core::McpRegistry> registry)
    : _config(std::move(config)), _registry(std::move(registry)) {}

void McpShard::_register_mcp_methods() {
    auto registry    = _registry;
    auto cfg_name    = _config.name;
    auto cfg_version = _config.version;

    // ── initialize ──────────────────────────────────────────────────────────
    _dispatcher.register_method("initialize",
        [cfg_name, cfg_version](const json&) -> seastar::future<json> {
            server_log.info("Client connected and initialized.");
            json caps;
            caps["tools"]     = {{"listChanged", false}};
            caps["resources"] = {{"listChanged", false}, {"subscribe", true}};
            caps["prompts"]   = {{"listChanged", false}};
            caps["logging"]   = json::object();
            json result;
            result["protocolVersion"] = "2025-11-25";
            result["serverInfo"]      = {{"name", cfg_name}, {"version", cfg_version}};
            result["capabilities"]    = std::move(caps);
            co_return result;
        });

    _dispatcher.register_notification("notifications/initialized",
        [](const json&) -> seastar::future<> { co_return; });

    // ── ping ─────────────────────────────────────────────────────────────────
    _dispatcher.register_method("ping",
        [](const json&) -> seastar::future<json> { co_return json::object(); });

    // ── tools/list ───────────────────────────────────────────────────────────
    _dispatcher.register_method("tools/list",
        [registry](const json& params) -> seastar::future<json> {
            auto all = registry->get_tools_list();
            // cursor-based pagination: cursor is a base-10 integer offset as string
            std::size_t offset = 0;
            if (params.contains("cursor") && params["cursor"].is_string()) {
                try { offset = std::stoull(params["cursor"].get<std::string>()); } catch (...) {}
            }
            constexpr std::size_t page_size = 50;
            json result;
            if (offset >= all.size()) {
                result["tools"] = json::array();
            } else {
                auto begin = all.begin() + static_cast<std::ptrdiff_t>(offset);
                auto end   = (offset + page_size < all.size())
                             ? begin + static_cast<std::ptrdiff_t>(page_size)
                             : all.end();
                result["tools"] = json(std::vector<json>(begin, end));
                if (end != all.end()) {
                    result["nextCursor"] = std::to_string(offset + page_size);
                }
            }
            co_return result;
        });

    // ── tools/call ───────────────────────────────────────────────────────────
    _dispatcher.register_method("tools/call",
        [registry](const json& params) -> seastar::future<json> {
            std::string name = params.value("name", "");
            server_log.info("Client calling tool: {}", name);
            co_return co_await registry->call_tool(name, params.value("arguments", json::object()));
        });

    // ── resources/list ───────────────────────────────────────────────────────
    _dispatcher.register_method("resources/list",
        [registry](const json& params) -> seastar::future<json> {
            auto all = registry->get_resources_list();
            std::size_t offset = 0;
            if (params.contains("cursor") && params["cursor"].is_string()) {
                try { offset = std::stoull(params["cursor"].get<std::string>()); } catch (...) {}
            }
            constexpr std::size_t page_size = 50;
            json result;
            if (offset >= all.size()) {
                result["resources"] = json::array();
            } else {
                auto begin = all.begin() + static_cast<std::ptrdiff_t>(offset);
                auto end   = (offset + page_size < all.size())
                             ? begin + static_cast<std::ptrdiff_t>(page_size)
                             : all.end();
                result["resources"] = json(std::vector<json>(begin, end));
                if (end != all.end()) {
                    result["nextCursor"] = std::to_string(offset + page_size);
                }
            }
            co_return result;
        });

    // ── resources/templates/list ─────────────────────────────────────────────
    _dispatcher.register_method("resources/templates/list",
        [](const json&) -> seastar::future<json> {
            co_return json{{"resourceTemplates", json::array({
                {
                    {"uriTemplate", "sys://metrics/{component}"},
                    {"name", "system_metrics_template"},
                    {"description", "动态获取系统各组件的指标（component 可为 cpu / disk / network）"}
                }
            })}};
        });

    // ── resources/read ───────────────────────────────────────────────────────
    _dispatcher.register_method("resources/read",
        [registry](const json& params) -> seastar::future<json> {
            std::string uri = params.value("uri", "");
            server_log.info("Client reading resource: {}", uri);
            if (uri.rfind("sys://metrics/", 0) == 0) {
                std::string comp = uri.substr(14);
                std::string content = "动态组件 [" + comp + "] 状态: 运行良好, 负载极低。";
                co_return json{{"contents", json::array({{
                    {"uri", uri}, {"mimeType", "text/plain"}, {"text", content}
                }})}};
            }
            co_return co_await registry->read_resource(uri);
        });

    // ── resources/subscribe ──────────────────────────────────────────────────
    _dispatcher.register_method("resources/subscribe",
        [this](const json& params) -> seastar::future<json> {
            std::string uri = params.value("uri", "");
            if (!uri.empty()) subscribe_resource(uri);
            co_return json::object();
        });

    // ── resources/unsubscribe ────────────────────────────────────────────────
    _dispatcher.register_method("resources/unsubscribe",
        [this](const json& params) -> seastar::future<json> {
            std::string uri = params.value("uri", "");
            if (!uri.empty()) unsubscribe_resource(uri);
            co_return json::object();
        });

    // ── prompts/list ─────────────────────────────────────────────────────────
    _dispatcher.register_method("prompts/list",
        [registry](const json& params) -> seastar::future<json> {
            auto all = registry->get_prompts_list();
            std::size_t offset = 0;
            if (params.contains("cursor") && params["cursor"].is_string()) {
                try { offset = std::stoull(params["cursor"].get<std::string>()); } catch (...) {}
            }
            constexpr std::size_t page_size = 50;
            json result;
            if (offset >= all.size()) {
                result["prompts"] = json::array();
            } else {
                auto begin = all.begin() + static_cast<std::ptrdiff_t>(offset);
                auto end   = (offset + page_size < all.size())
                             ? begin + static_cast<std::ptrdiff_t>(page_size)
                             : all.end();
                result["prompts"] = json(std::vector<json>(begin, end));
                if (end != all.end()) {
                    result["nextCursor"] = std::to_string(offset + page_size);
                }
            }
            co_return result;
        });

    // ── prompts/get ──────────────────────────────────────────────────────────
    _dispatcher.register_method("prompts/get",
        [registry](const json& params) -> seastar::future<json> {
            std::string name = params.value("name", "");
            server_log.info("Client getting prompt: {}", name);
            co_return co_await registry->get_prompt(name, params.value("arguments", json::object()));
        });

    // ── logging/setLevel ─────────────────────────────────────────────────────
    _dispatcher.register_method("logging/setLevel",
        [this](const json& params) -> seastar::future<json> {
            std::string level = params.value("level", "info");
            seastar::log_level sl = seastar::log_level::info;
            if      (level == "debug" || level == "trace") sl = seastar::log_level::debug;
            else if (level == "info")                       sl = seastar::log_level::info;
            else if (level == "warning" || level == "warn") sl = seastar::log_level::warn;
            else if (level == "error" || level == "critical" || level == "alert" ||
                     level == "emergency")                  sl = seastar::log_level::error;
            seastar::global_logger_registry().set_all_loggers_level(sl);
            // 同步更新本核及所有核的 MCP 客户端日志级别
            set_client_log_level(sl);
            co_await container().invoke_on_others([sl](McpShard& s) {
                s.set_client_log_level(sl);
                return seastar::make_ready_future<>();
            });
            server_log.info("Log level set to: {}", level);
            co_return json::object();
        });

    // ── roots/list ───────────────────────────────────────────────────────────
    _dispatcher.register_method("roots/list",
        [](const json&) -> seastar::future<json> {
            co_return json{{"roots", json::array()}};
        });

    // ── notifications/cancelled ──────────────────────────────────────────────
    _dispatcher.register_notification("notifications/cancelled",
        [](const json& params) -> seastar::future<> {
            std::string req_id = params.contains("requestId")
                                 ? params["requestId"].dump() : "(unknown)";
            std::string reason = params.value("reason", "");
            server_log.info("Request {} cancelled: {}", req_id, reason);
            co_return;
        });

    // ── completion/complete ───────────────────────────────────────────────────
    _dispatcher.register_method("completion/complete",
        [](const json& params) -> seastar::future<json> {
            json ref      = params.value("ref", json::object());
            json argument = params.value("argument", json::object());
            json values   = json::array();
            if (ref.value("type", "")      == "prompt" &&
                ref.value("name", "")      == "analyze_server_health" &&
                argument.value("name", "") == "focus") {
                std::string input = argument.value("value", "");
                for (const char* opt : {"memory", "cpu", "disk", "network"}) {
                    if (std::string(opt).rfind(input, 0) == 0) values.push_back(opt);
                }
            }
            co_return json{{"completion", {
                {"values", values}, {"total", values.size()}, {"hasMore", false}
            }}};
        });
}

// ── McpServer ────────────────────────────────────────────────────────────────

seastar::future<> McpServer::start() {
    // 1. 在所有核心上启动 McpShard（每核各注册一次 MCP 方法）
    co_await _shards.start(_config, _registry);
    co_await _shards.invoke_on_all(&McpShard::start);
    server_log.info("McpShards started on {} cores", seastar::smp::count);

    // 2. 根据配置创建并启动 transport
    if (_config.enable_http) {
        auto t = std::make_unique<mcp::transport::HttpSseTransport>(_config.http_port);
        co_await t->start(*this);
        _transports.push_back(std::move(t));
    }
    if (_config.enable_streamable_http) {
        auto t = std::make_unique<mcp::transport::StreamableHttpTransport>(_config.streamable_http_port);
        co_await t->start(*this);
        _transports.push_back(std::move(t));
    }
    if (_config.enable_stdio) {
        auto t = std::make_unique<mcp::transport::StdioTransport>();
        co_await t->start(*this);
        _transports.push_back(std::move(t));
    }
}

seastar::future<> McpServer::stop() {
    // 先停 transports（停止接受新请求）
    for (auto& t : _transports) {
        co_await t->stop();
    }
    _transports.clear();
    // 再停 shards（关闭 SSE sessions，释放 dispatcher）
    co_await _shards.stop();
}

} // namespace mcp::server
