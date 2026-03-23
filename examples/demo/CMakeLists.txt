cmake_minimum_required(VERSION 3.15)

# mcp_sdk 库已在父 CMakeLists.txt 中定义，此处只编译示例可执行文件
add_executable(demo_server
    main.cc
)

target_include_directories(demo_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(demo_server PRIVATE mcp_sdk::mcp_sdk)
