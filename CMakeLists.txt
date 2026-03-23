cmake_minimum_required(VERSION 3.15)
project(SeastarMcpSdk CXX)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# C++20 是必须的（Seastar 协程依赖）
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── 依赖 ────────────────────────────────────────────────────────────────────

find_package(Seastar REQUIRED)

find_package(nlohmann_json 3.11 REQUIRED)

# ─── SDK 库 target ───────────────────────────────────────────────────────────

add_library(mcp_sdk OBJECT
    src/mcp/server/mcp_server.cc
    src/seastar_patches/http_common_patch.cc
)

target_include_directories(mcp_sdk PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(mcp_sdk PUBLIC
    Seastar::seastar
    nlohmann_json::nlohmann_json
)

# Alias target：使用者可通过 mcp_sdk::mcp_sdk 引入
add_library(mcp_sdk::mcp_sdk ALIAS mcp_sdk)

# ─── 示例程序（可选）────────────────────────────────────────────────────────

option(MCP_BUILD_EXAMPLES "Build example applications" ON)
if(MCP_BUILD_EXAMPLES)
    add_subdirectory(examples/demo)
endif()

enable_testing()
add_subdirectory(tests)
