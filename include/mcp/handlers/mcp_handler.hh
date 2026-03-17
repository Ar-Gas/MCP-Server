#pragma once
#include "mcp/server/mcp_server.hh"
#include "mcp/tools/mcp_tool.hh"
#include <unordered_map>
#include <memory>
#include <nlohmann/json.hpp>

namespace mcp::handlers {

class ToolManager {
    std::unordered_map<std::string, std::shared_ptr<mcp::tools::McpTool>> _tools;

public:
    void register_tool(std::shared_ptr<mcp::tools::McpTool> tool) {
        _tools[tool->get_name()] = std::move(tool);
    }

    nlohmann::json get_all_tools_list() const {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& [name, tool] : _tools) {
            list.push_back(tool->get_definition());
        }
        return list;
    }

    seastar::future<nlohmann::json> call_tool(const std::string& name, const nlohmann::json& args) {
        auto it = _tools.find(name);
        if (it != _tools.end()) {
            return it->second->execute(args);
        } else {
            nlohmann::json err;
            err["isError"] = true;
            err["content"] = nlohmann::json::array({{{"type", "text"}, {"text", "未知的工具: " + name}}});
            return seastar::make_ready_future<nlohmann::json>(std::move(err));
        }
    }
};

class McpHandler {
public:
    static void register_routes(mcp::server::McpServer& server);
};

} // namespace mcp::handlers