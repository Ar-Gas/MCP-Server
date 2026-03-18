#pragma once
#include "mcp/server/mcp_server.hh"
#include "mcp/interfaces.hh"
#include <unordered_map>
#include <memory>
#include <nlohmann/json.hpp>

namespace mcp::handlers {

// ================== 统一资源管理器 ==================
class McpRegistry {
    std::unordered_map<std::string, std::shared_ptr<mcp::interfaces::McpTool>> _tools;
    std::unordered_map<std::string, std::shared_ptr<mcp::interfaces::McpResource>> _resources;
    std::unordered_map<std::string, std::shared_ptr<mcp::interfaces::McpPrompt>> _prompts;

public:
    void register_tool(std::shared_ptr<mcp::interfaces::McpTool> tool) { _tools[tool->get_name()] = std::move(tool); }
    void register_resource(std::shared_ptr<mcp::interfaces::McpResource> res) { _resources[res->get_uri()] = std::move(res); }
    void register_prompt(std::shared_ptr<mcp::interfaces::McpPrompt> prompt) { _prompts[prompt->get_name()] = std::move(prompt); }

    nlohmann::json get_tools_list() const {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& [_, tool] : _tools) list.push_back(tool->get_definition());
        return list;
    }

    nlohmann::json get_resources_list() const {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& [_, res] : _resources) list.push_back(res->get_definition());
        return list;
    }

    nlohmann::json get_prompts_list() const {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& [_, p] : _prompts) list.push_back(p->get_definition());
        return list;
    }

    seastar::future<nlohmann::json> call_tool(const std::string& name, const nlohmann::json& args) {
        if (_tools.contains(name)) return _tools[name]->execute(args);
        return seastar::make_ready_future<nlohmann::json>(nlohmann::json{{"isError", true}, {"content", nlohmann::json::array({{{"type", "text"}, {"text", "Tool not found"}}})}});
    }

    seastar::future<nlohmann::json> read_resource(const std::string& uri) {
        if (_resources.contains(uri)) {
            return _resources[uri]->read().then([uri](std::string content) {
                return nlohmann::json{{"contents", nlohmann::json::array({{{"uri", uri}, {"mimeType", "text/plain"}, {"text", content}}})}};
            });
        }
        return seastar::make_ready_future<nlohmann::json>(nlohmann::json{{"error", "Resource not found"}});
    }

    seastar::future<nlohmann::json> get_prompt(const std::string& name, const nlohmann::json& args) {
        if (_prompts.contains(name)) {
            return _prompts[name]->get_messages(args).then([](nlohmann::json msgs) {
                return nlohmann::json{{"description", "Prompt execution"}, {"messages", msgs}};
            });
        }
        return seastar::make_ready_future<nlohmann::json>(nlohmann::json{{"error", "Prompt not found"}});
    }
};

class McpHandler {
public:
    static void register_routes(mcp::server::McpServer& server);
};

} // namespace mcp::handlers