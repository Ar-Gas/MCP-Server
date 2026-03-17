这是一份为你量身定制的、专业且极客范儿十足的 `README.md`。

这份 README 不仅涵盖了项目的技术亮点、依赖说明、构建步骤，还为你和未来的协作者（包括 AI 客户端）提供了详细的测试命令和扩展指南。

你可以直接将以下内容复制并保存为项目根目录的 `README.md` 文件：

---

# 🚀 Seastar MCP Server

基于 **C++20** 和 **Seastar 异步框架** 构建的高性能 **MCP (Model Context Protocol) 服务器**。

本项目旨在为大语言模型（如 Claude, Cursor 等）提供标准化的本地/远程工具调用（Tool Calling）能力。结合 Seastar 框架的 Share-Nothing 架构与 C++20 协程，能够以极低的延迟和极高的吞吐量处理 AI 客户端的并发请求。

## ✨ 核心特性

- **⚡ 极致性能**：底层基于 Seastar 异步 I/O 框架，单核性能炸裂。
- **🧵 现代 C++**：全面拥抱 C++20 协程 (`co_await` / `co_return`)，告别回调地狱。
- **🔌 灵活的插件系统**：高度解耦的 `ToolManager`，新增 AI 工具只需实现基类并注册，零 `if-else`。
- **📡 标准协议**：严格遵循 [MCP 规范](https://modelcontextprotocol.io/) 与 JSON-RPC 2.0 协议。

---

## 📂 项目结构

```text
.
├── app
│   └── main.cc                        # 程序入口：启动 Seastar 引擎，注册路由，处理退出信号
├── include
│   └── mcp
│       ├── handlers
│       │   └── mcp_handler.hh         # ToolManager 定义与路由注册入口声明
│       ├── protocol
│       │   └── json_rpc.hh            # JSON-RPC 2.0 协议的数据结构定义
│       ├── router
│       │   └── dispatcher.hh          # 请求分发器：解析 JSON，自动路由 Method/Notification
│       ├── server
│       │   └── mcp_server.hh          # 基于 seastar::httpd 的 Web 服务器封装
│       └── tools
│           ├── mcp_tool.hh            # 核心接口：McpTool 抽象基类
│           ├── calculate_sum_tool.hh  # 示例工具 1：加法计算器
│           └── get_current_time_tool.hh # 示例工具 2：获取系统当前时间
├── src
│   └── mcp
│       └── handlers
│           └── mcp_handler.cc         # 核心业务逻辑：绑定握手协议、工具列表与调用
├── third                              # 第三方依赖 (Seastar 源码等)
└── CMakeLists.txt                     # CMake 构建配置文件
```

---

## 🛠️ 环境与依赖

在编译本项目之前，请确保您的系统满足以下条件：

1. **操作系统**：Linux (Ubuntu 20.04/22.04 推荐) 或 WSL2。
2. **编译器**：支持 C++20 的 GCC (>= 10.0) 或 Clang (>= 10.0)。
3. **构建工具**：CMake (>= 3.15) 和 Make / Ninja。
4. **Seastar 框架**：依赖 `third` 目录下的 Seastar 源码，或已在系统中全局安装 Seastar 开发库。
5. **nlohmann/json**：用于 JSON 解析（CMake 会自动拉取，无需手动安装）。

---

## 🚀 编译与运行

**1. 创建构建目录并编译**

```bash
mkdir build
cd build
# 配置 CMake 
cmake .. -G Ninja   # 如果没有安装 Ninja，直接运行 cmake .. 即可
# 编译项目
ninja               # 或使用 make -j$(nproc)
```

**2. 启动 MCP 服务器**

由于底层是 Seastar 框架，推荐指定 `-c` 参数来分配 CPU 核心数：

```bash
# 以 1 个 CPU 核心启动服务器
./mcp_server -c1
```

看到输出 `MCP Server is running on port 8080.` 即表示启动成功！使用 `Ctrl+C` 可以优雅停机。

---

## 🎮 使用与测试指南

服务器运行在 `http://127.0.0.1:8080/message`，接受标准 JSON-RPC 2.0 POST 请求。请打开**另一个终端窗口**，使用 `curl` 模拟 AI 客户端进行测试。

### 1. 🤝 协议握手 (Initialize)

客户端连接时的第一步，验证协议版本并获取服务器能力（Capabilities）。

```bash
curl -X POST http://127.0.0.1:8080/message \
     -H "Content-Type: application/json" \
     -d '{
           "jsonrpc": "2.0",
           "id": 1,
           "method": "initialize",
           "params": {
               "protocolVersion": "2024-11-05",
               "capabilities": {},
               "clientInfo": {"name": "TestClient", "version": "1.0.0"}
           }
         }' | jq
```

### 2. 📋 获取工具列表 (tools/list)

大模型会请求此接口，了解当前服务器提供了哪些能力。

```bash
curl -X POST http://127.0.0.1:8080/message \
     -H "Content-Type: application/json" \
     -d '{
           "jsonrpc": "2.0",
           "id": 2,
           "method": "tools/list",
           "params": {}
         }' | jq
```

### 3. 🛠️ 调用工具 (tools/call)

**示例 A：调用系统时间工具 (`get_current_time`)**

```bash
curl -X POST http://127.0.0.1:8080/message \
     -H "Content-Type: application/json" \
     -d '{
           "jsonrpc": "2.0",
           "id": 3,
           "method": "tools/call",
           "params": {
               "name": "get_current_time",
               "arguments": {}
           }
         }' | jq
```

**示例 B：调用加法计算器 (`calculate_sum`)**

```bash
curl -X POST http://127.0.0.1:8080/message \
     -H "Content-Type: application/json" \
     -d '{
           "jsonrpc": "2.0",
           "id": 4,
           "method": "tools/call",
           "params": {
               "name": "calculate_sum",
               "arguments": {
                   "a": 15.5,
                   "b": 24.5
               }
           }
         }' | jq
```

---

## 🧩 如何开发新工具？

得益于高度解耦的架构设计，为您的大模型添加新能力极其简单，仅需两步：

**步骤 1：新建一个工具类继承 `McpTool`**
实现三个纯虚函数：`get_name()`, `get_definition()`, `execute()`。
（参考 `include/mcp/tools/calculate_sum_tool.hh`）

**步骤 2：在 `mcp_handler.cc` 中注册它**
```cpp
// 引入你的头文件
#include "mcp/tools/my_awesome_tool.hh"

// 在 McpHandler::register_routes 中注册：
tool_manager->register_tool(std::make_shared<mcp::tools::MyAwesomeTool>());
```
重新编译运行，系统会自动将其暴露给所有的 AI 客户端，无需改动任何网络路由代码！