cmake_minimum_required(VERSION 3.15)
project(SeastarMcpServer CXX)

# 【新增这两行】消除 FetchContent 警告
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# Seastar 必须使用 C++20 才能开启协程 (co_await)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 1. 查找 Seastar 库 (请确保系统已安装 Seastar)
find_package(Seastar REQUIRED)

# 2. 自动拉取并配置 nlohmann_json (无需手动安装)
include(FetchContent)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# 3. 设置头文件搜索路径
include_directories(include)

# 4. 编译可执行文件
add_executable(mcp_server 
    app/main.cc
    src/mcp/handlers/mcp_handler.cc
)

# 5. 链接 Seastar 和 JSON 库
target_link_libraries(mcp_server PRIVATE 
    Seastar::seastar 
    nlohmann_json::nlohmann_json
)