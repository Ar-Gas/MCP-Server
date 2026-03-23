#pragma once
#include "mcp/core/interfaces.hh"
#include "mcp/core/registry.hh"
#include "mcp/server/mcp_server.hh"
#include <memory>
#include <string>

namespace mcp {

// McpServerBuilder：流式 API，配置并构建 McpServer
class McpServerBuilder {
    server::McpServerConfig _config;
    std::shared_ptr<core::McpRegistry> _registry;

public:
    McpServerBuilder() : _registry(std::make_shared<core::McpRegistry>()) {}

    McpServerBuilder& name(std::string n) {
        _config.name = std::move(n);
        return *this;
    }
    McpServerBuilder& version(std::string v) {
        _config.version = std::move(v);
        return *this;
    }
    McpServerBuilder& with_http(uint16_t port = 8080) {
        _config.enable_http = true;
        _config.http_port = port;
        return *this;
    }
    McpServerBuilder& with_stdio() {
        _config.enable_stdio = true;
        return *this;
    }
    McpServerBuilder& with_streamable_http(uint16_t port = 8081) {
        _config.enable_streamable_http = true;
        _config.streamable_http_port = port;
        return *this;
    }

    template<typename T, typename... Args>
    McpServerBuilder& add_tool(Args&&... args) {
        _registry->register_tool(std::make_shared<T>(std::forward<Args>(args)...));
        return *this;
    }

    template<typename T, typename... Args>
    McpServerBuilder& add_resource(Args&&... args) {
        _registry->register_resource(std::make_shared<T>(std::forward<Args>(args)...));
        return *this;
    }

    template<typename T, typename... Args>
    McpServerBuilder& add_prompt(Args&&... args) {
        _registry->register_prompt(std::make_shared<T>(std::forward<Args>(args)...));
        return *this;
    }

    std::unique_ptr<server::McpServer> build() {
        return std::make_unique<server::McpServer>(std::move(_config), std::move(_registry));
    }
};

} // namespace mcp
