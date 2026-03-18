# 🚀 Seastar MCP Server

基于 **C++20** 和 **Seastar 异步框架** 构建的高性能、全功能 **MCP (Model Context Protocol) 服务器**。

本项目不仅将 C++ 的极致性能与大模型工具调用相结合，还完整实现了 MCP 规范的核心特性。得益于 Seastar 的 Share-Nothing 架构与底层的 StdIO 桥接技术，它既能作为高并发的 HTTP/SSE 微服务运行，也能作为轻量级的本地 StdIO 插件直接接入各种 AI 客户端。

---

## ✨ 核心特性

- **⚡ 极致性能**：基于 Seastar 异步 I/O 框架，单核支撑海量并发。在 2核4G 虚拟机上实测单节点稳定并发 1500，峰值 QPS 破万，P95 延迟低于 85ms。
- **🧵 现代 C++**：全面拥抱 C++20 协程 (`co_await` / `co_return`)，告别回调地狱，代码如丝般顺滑。
- **📦 全面支持 MCP 规范**：
  - 🛠️ **Tools (工具)**：提供计算、系统时间等动作执行。
  - 📊 **Resources (资源)**：支持静态资源读取（如内存状态）和动态资源模板（URI Templates）。
  - 📝 **Prompts (提示词)**：支持向 AI 下发带有上下文的系统提示词模板。
  - 💡 **Auto-Completion (自动补全)**：支持客户端参数联想提示。
- **🔌 双模通信 (Dual Transport)**：
  - **StdIO 模式**：原生兼容 Claude Desktop、Cursor、MCP Inspector 等标准客户端。
  - **HTTP/SSE 模式**：内置 Web Server 支持基于 TCP 端口的远程调用。

---

## 📂 项目结构

高度解耦的目录设计，新增 AI 能力只需实现对应的基类并注册，零 `if-else`。

```text
.
├── app
│   └── main.cc                        # 引擎入口与 StdIO-HTTP 桥接器
├── include/mcp
│   ├── handlers/mcp_handler.hh        # 统一资源注册表与路由核心
│   ├── interfaces.hh                  # McpTool, McpResource, McpPrompt 抽象基类
│   ├── prompts/analyze_system_prompt.hh # 示例：性能分析提示词
│   ├── protocol/json_rpc.hh           # JSON-RPC 2.0 协议支持
│   ├── resources/system_info_resource.hh# 示例：系统内存资源状态读取
│   ├── router/dispatcher.hh           # 异步 RPC 路由器
│   ├── server/mcp_server.hh           # HTTP POST & SSE 核心服务器
│   └── tools/                         # 示例：加法器与时间获取工具
├── src/mcp/handlers/mcp_handler.cc    # 路由绑定与业务分发逻辑
├── Dockerfile                         # 面向生产环境的极简镜像构建配置
└── CMakeLists.txt                     # CMake 构建文件 (自动拉取 nlohmann/json)
```

---

## 🛠️ 编译与构建

系统要求：Linux (Ubuntu 24.04 推荐) 或 WSL2，支持 C++20 的编译器 (GCC 10+ / Clang 10+)，CMake 3.15+。

**1. 本地直接编译**
```bash
mkdir build && cd build
cmake .. -G Ninja
ninja
```

**2. 使用 Docker 容器化构建（推荐）**
```bash
# 构建镜像 (体积小，包含最小化运行时依赖)
docker build -t seastar-mcp-server:v1 .

# 打包为离线压缩包 (可选)
docker save seastar-mcp-server:v1 | gzip > my-seastar-mcp.tar.gz
```

---

## 🎮 调试与运行

### 方法一：使用 MCP 官方 Inspector（强烈推荐）

官方提供的 Inspector 是最好的调试可视化工具，它可以通过 StdIO 直接启动你的 C++ 服务器。
在项目根目录（确保 build 目录下已生成 `mcp_server`）运行：

```bash
npx @modelcontextprotocol/inspector ./build/mcp_server
```
运行后浏览器将自动打开本地网页，你可以通过图形化界面点击测试所有的 Tools、Resources 和 Prompts，并实时查看 JSON-RPC 交互日志！

### 方法二：作为独立微服务运行 (HTTP/SSE)

通过指定 Seastar 参数启动（限制 1 个 CPU 核心与 512MB 内存）：
```bash
./build/mcp_server -c 1 -m 512M --default-log-level=warn
```
启动后，服务器会在 `127.0.0.1:8080` 监听。你可以使用 `curl` 进行测试：

<details>
<summary>展开查看 Curl 测试命令</summary>

**获取服务器提供的工具列表**
```bash
curl -X POST http://127.0.0.1:8080/message \
     -d '{"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}}'
```

**读取系统状态资源 (Resources)**
```bash
curl -X POST http://127.0.0.1:8080/message \
     -d '{"jsonrpc": "2.0", "id": 2, "method": "resources/read", "params": {"uri": "sys://memory-info"}}'
```

**请求提示词模板 (Prompts)**
```bash
curl -X POST http://127.0.0.1:8080/message \
     -d '{"jsonrpc": "2.0", "id": 3, "method": "prompts/get", "params": {"name": "analyze_server_health", "arguments": {"focus": "memory"}}}'
```
</details>

---

## 🤖 接入 AI 客户端 (Claude Desktop)

本项目完美支持作为本地工具链接入大模型客户端。你可以修改 Claude Desktop 的配置文件（通常在 `~/.claude/claude_desktop_config.json` 或 Windows 的 `%APPDATA%\Claude\claude_desktop_config.json`）。

### 方式 A：通过本地二进制直连
```json
{
  "mcpServers": {
    "seastar_cpp_server": {
      "command": "/绝对路径/到你的/项目/build/mcp_server",
      "args": ["-c", "1", "-m", "512M", "--default-log-level=warn"]
    }
  }
}
```

### 方式 B：通过 Docker 运行（沙盒隔离更安全）
如果你打包了 Docker 镜像，可以通过下面的方式接入：
```json
{
  "mcpServers": {
    "seastar_docker_tools": {
      "command": "docker",
      "args": [
        "run",
        "-i",         
        "--rm",       
        "seastar-mcp-server:v1"
      ]
    }
  }
}
```
*(注意：`docker run` 必须包含 `-i` 才能保持 StdIO 输入流的开启。)*

---

## 🧩 如何扩展你的业务？

为你的服务器添加新能力非常简单：
1. **继承基类**：在 `include/mcp/tools/` 或 `resources/` 下新建头文件，继承 `McpTool` 或 `McpResource` 等基类并实现虚函数。
2. **注册路由**：在 `src/mcp/handlers/mcp_handler.cc` 中的 `register_routes` 方法里，调用 `registry->register_xxx(...)`。
3. **编译生效**：无需修改任何网络底层代码，新功能即刻上线！