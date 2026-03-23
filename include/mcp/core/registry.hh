#pragma once
#include "mcp/core/interfaces.hh"
#include "mcp/protocol/json_rpc.hh"
#include <seastar/core/future.hh>
#include <seastar/core/coroutine.hh>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <memory>
#include <string>

namespace mcp::core {

using json = nlohmann::json;

// 统一资源注册表：管理 Tools、Resources、Prompts
class McpRegistry {
    std::unordered_map<std::string, std::shared_ptr<McpTool>>     _tools;
    std::unordered_map<std::string, std::shared_ptr<McpResource>> _resources;
    std::unordered_map<std::string, std::shared_ptr<McpPrompt>>   _prompts;

public:
    void register_tool(std::shared_ptr<McpTool> tool) {
        _tools[tool->get_name()] = std::move(tool);
    }
    void register_resource(std::shared_ptr<McpResource> res) {
        _resources[res->get_uri()] = std::move(res);
    }
    void register_prompt(std::shared_ptr<McpPrompt> prompt) {
        _prompts[prompt->get_name()] = std::move(prompt);
    }

    json get_tools_list() const {
        json list = json::array();
        for (const auto& [_, tool] : _tools) {
            json def = tool->get_definition();
            if (!def.contains("title")) def["title"] = tool->get_title();
            if (auto schema = tool->get_output_schema()) def["outputSchema"] = *schema;
            if (!tool->get_annotations().is_null()) def["annotations"] = tool->get_annotations();
            if (auto icon = tool->get_icon_uri()) def["icon"] = *icon;
            list.push_back(std::move(def));
        }
        return list;
    }

    json get_resources_list() const {
        json list = json::array();
        for (const auto& [_, res] : _resources) {
            json def = res->get_definition();
            if (!def.contains("title")) def["title"] = res->get_title();
            if (auto icon = res->get_icon_uri()) def["icon"] = *icon;
            list.push_back(std::move(def));
        }
        return list;
    }

    json get_prompts_list() const {
        json list = json::array();
        for (const auto& [_, p] : _prompts) {
            json def = p->get_definition();
            if (!def.contains("title")) def["title"] = p->get_title();
            if (auto icon = p->get_icon_uri()) def["icon"] = *icon;
            list.push_back(std::move(def));
        }
        return list;
    }

    seastar::future<json> call_tool(const std::string& name, const json& args) {
        auto it = _tools.find(name);
        if (it == _tools.end()) {
            throw mcp::protocol::JsonRpcException(
                mcp::protocol::JsonRpcErrorCode::InvalidParams,
                "Tool not found: " + name);
        }
        json result = co_await it->second->execute(args);
        // MCP 规范要求成功响应包含 isError: false
        if (!result.contains("isError")) result["isError"] = false;
        // structuredContent 若工具已填写则透传，无需额外处理
        co_return result;
    }

    seastar::future<json> read_resource(const std::string& uri) {
        auto it = _resources.find(uri);
        if (it == _resources.end()) {
            throw mcp::protocol::JsonRpcException(
                mcp::protocol::JsonRpcErrorCode::InvalidParams,
                "Resource not found: " + uri);
        }
        std::string content = co_await it->second->read();
        co_return json{{"contents", json::array({{
            {"uri", uri},
            {"mimeType", it->second->get_definition().value("mimeType", "text/plain")},
            {"text", content}
        }})}};
    }

    seastar::future<json> get_prompt(const std::string& name, const json& args) {
        auto it = _prompts.find(name);
        if (it == _prompts.end()) {
            throw mcp::protocol::JsonRpcException(
                mcp::protocol::JsonRpcErrorCode::InvalidParams,
                "Prompt not found: " + name);
        }
        json msgs = co_await it->second->get_messages(args);
        co_return json{{"description", it->second->get_definition().value("description", "")}, {"messages", msgs}};
    }
};

} // namespace mcp::core
