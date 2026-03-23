#pragma once
#include <seastar/core/future.hh>
#include <nlohmann/json.hpp>
#include <string>
#include <optional>

namespace mcp::core {

// ================== Tool 接口 ==================
class McpTool {
public:
    virtual ~McpTool() = default;
    virtual std::string get_name() const = 0;
    virtual std::string get_title() const { return get_name(); }
    virtual nlohmann::json get_definition() const = 0;
    virtual seastar::future<nlohmann::json> execute(const nlohmann::json& args) = 0;
    virtual std::optional<nlohmann::json> get_output_schema() const { return std::nullopt; }
    virtual nlohmann::json get_annotations() const { return nullptr; }
    virtual std::optional<std::string> get_icon_uri() const { return std::nullopt; }
};

// ================== Resource 接口 ==================
class McpResource {
public:
    virtual ~McpResource() = default;
    virtual std::string get_uri() const = 0;
    virtual std::string get_name() const = 0;
    virtual std::string get_title() const { return get_name(); }
    virtual nlohmann::json get_definition() const = 0;
    virtual seastar::future<std::string> read() = 0;
    virtual std::optional<std::string> get_icon_uri() const { return std::nullopt; }
};

// ================== Prompt 接口 ==================
class McpPrompt {
public:
    virtual ~McpPrompt() = default;
    virtual std::string get_name() const = 0;
    virtual std::string get_title() const { return get_name(); }
    virtual nlohmann::json get_definition() const = 0;
    virtual seastar::future<nlohmann::json> get_messages(const nlohmann::json& args) = 0;
    virtual std::optional<std::string> get_icon_uri() const { return std::nullopt; }
};

} // namespace mcp::core
