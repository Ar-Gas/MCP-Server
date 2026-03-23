# 构建与部署

---

## 1. 系统要求

| 组件 | 最低版本 | 说明 |
|---|---|---|
| Linux 内核 | 5.4+ | io_uring 需要 5.1+；epoll 需要 5.4+ |
| GCC | 11+ | C++20 协程支持 |
| CMake | 3.15+ | |
| Seastar | 23.x | 见下文安装方法 |
| nlohmann/json | 3.11+ | |
| Boost | 1.74+ | 单元测试框架 |
| Python | 3.8+ | 集成测试（可选） |

**测试环境**：Ubuntu 22.04 / 24.04，GCC 12，Linux 6.8

---

## 2. 安装依赖

### 2.1 Seastar

Seastar 需要从源码编译，或使用预编译包：

```bash
# Ubuntu 22.04/24.04 通过包管理器安装（如有）
apt-get install seastar-dev

# 或从源码编译（推荐）
git clone https://github.com/scylladb/seastar.git
cd seastar
./install-dependencies.sh         # 安装 Seastar 的所有依赖
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DSeastar_INSTALL=ON
make -j$(nproc)
sudo make install
```

### 2.2 nlohmann/json

```bash
# Ubuntu
sudo apt-get install nlohmann-json3-dev  # >= 3.11

# 或手动安装
wget https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
sudo mkdir -p /usr/local/include/nlohmann
sudo cp json.hpp /usr/local/include/nlohmann/
```

### 2.3 其他依赖

```bash
sudo apt-get install -y \
    build-essential \
    cmake           \
    libboost-all-dev \
    python3-pip

# 集成测试额外依赖
pip3 install pytest pytest-json-report requests
```

---

## 3. 克隆与构建

### 3.1 克隆仓库

```bash
git clone https://github.com/your-org/seastar-mcp-server.git
cd seastar-mcp-server
```

### 3.2 配置构建目录

```bash
mkdir build && cd build
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DMCP_BUILD_EXAMPLES=ON    \
    -DMCP_BUILD_TESTS=ON
```

**CMake 选项**：

| 选项 | 默认 | 说明 |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Debug` | `Debug` / `Release` / `RelWithDebInfo` |
| `MCP_BUILD_EXAMPLES` | `ON` | 构建 examples/demo |
| `MCP_BUILD_TESTS` | `ON` | 构建 C++ 单元测试 |

### 3.3 编译

```bash
# 编译全部（SDK + demo + tests）
ninja

# 仅编译 demo_server
ninja demo_server

# 仅编译 C++ 单元测试
ninja test_dispatcher test_registry
```

**编译产物**：

```
build/
├── examples/demo/demo_server      # 示例 Server 可执行文件
├── tests/test_dispatcher          # JSON-RPC 调度器单元测试
└── tests/test_registry            # Registry 单元测试
```

---

## 4. 运行 demo_server

### 4.1 基本运行

```bash
./examples/demo/demo_server -c1 -m256M --overprovisioned
```

输出：
```
[INFO] seastar-mcp-demo running (HTTP/SSE :8080, Streamable HTTP :8081, StdIO)
```

### 4.2 完整参数说明

```
demo_server [seastar-options]

常用 Seastar 参数：
  -c N, --smp=N             使用 N 个 CPU 核心（默认：全部）
  -m SIZE, --memory=SIZE    内存配额（如 256M / 1G / 2048M）
  --overprovisioned         允许 Seastar 与其他进程共享 CPU
                            （非独占服务器必须开启，否则 Seastar 会 spin-wait）
  --default-log-level=LEVEL Seastar 内部日志级别
                            取值：trace / debug / info / warn / error
  --reactor-backend=BACKEND I/O 后端（epoll / io_uring，默认 epoll）
  --task-quota-ms=N         每个任务最长执行时间（ms），超出触发抢占
  --poll-mode               禁用睡眠，全力轮询（高性能专用）
  -h, --help                打印帮助
```

### 4.3 常用场景

```bash
# 开发调试：单核，所有日志
./demo_server -c1 -m256M --overprovisioned --default-log-level=debug

# 生产环境：4核，抑制无关日志
./demo_server -c4 -m512M --overprovisioned --default-log-level=warn

# 最高性能：全核，io_uring 后端，关闭睡眠
./demo_server -c$(nproc) -m2G --reactor-backend=io_uring --poll-mode

# 只允许使用 CPU 0-3（taskset 隔离）
taskset -c 0-3 ./demo_server -c4 -m512M --overprovisioned
```

---

## 5. Docker 部署

仓库提供了 `Dockerfile`：

```bash
# 构建镜像
docker build -t seastar-mcp-server .

# 运行（需要 --privileged 或相应的 capabilities，因为 Seastar 需要设置调度策略）
docker run --rm -p 8080:8080 -p 8081:8081 \
    --privileged \
    seastar-mcp-server \
    -c2 -m512M --overprovisioned --default-log-level=warn
```

**Dockerfile 要点**：
- 基于 Ubuntu 22.04
- 预安装所有依赖
- 从源码编译 Seastar 和 demo_server
- `ENTRYPOINT` 为 `demo_server`

---

## 6. 配置自定义 Server

### 6.1 在自己的项目中引入 SDK

**CMakeLists.txt**：

```cmake
cmake_minimum_required(VERSION 3.15)
project(MyMcpServer CXX)
set(CMAKE_CXX_STANDARD 20)

# 方式 A：add_subdirectory（源码引入）
add_subdirectory(path/to/seastar-mcp-server)

add_executable(my_server main.cc)
target_link_libraries(my_server PRIVATE mcp_sdk::mcp_sdk)
```

或：

```cmake
# 方式 B：find_package（安装后引入）
find_package(SeastarMcpSdk REQUIRED)
target_link_libraries(my_server PRIVATE mcp_sdk::mcp_sdk)
```

### 6.2 头文件引入

用户代码只需一行：

```cpp
#include <mcp/mcp.hh>
```

---

## 7. 系统调优（生产环境）

### 7.1 CPU 隔离

```bash
# isolcpus 内核参数（需重启）
GRUB_CMDLINE_LINUX="isolcpus=0-3 nohz_full=0-3 rcu_nocbs=0-3"

# 将进程绑定到隔离核
taskset -c 0-3 ./demo_server -c4 -m512M
```

### 7.2 NUMA 感知

```bash
# 在 NUMA 节点 0 上运行，绑定内存
numactl --cpunodebind=0 --membind=0 ./demo_server -c8 -m8G
```

### 7.3 网络调优

```bash
# 增大 TCP 收发缓冲区
sysctl -w net.core.rmem_max=16777216
sysctl -w net.core.wmem_max=16777216
sysctl -w net.ipv4.tcp_rmem='4096 87380 16777216'
sysctl -w net.ipv4.tcp_wmem='4096 65536 16777216'

# 关闭 SELinux（如不需要）
setenforce 0
```

### 7.4 io_uring 权限

```bash
# 允许非 root 用户使用 io_uring
sysctl -w kernel.io_uring_disabled=0

# 或直接以 root 运行（不推荐生产）
```

---

## 8. 进程管理（systemd）

`/etc/systemd/system/mcp-server.service`：

```ini
[Unit]
Description=Seastar MCP Server
After=network.target

[Service]
Type=simple
User=mcp
ExecStart=/usr/local/bin/demo_server \
    -c4 -m512M --overprovisioned \
    --default-log-level=warn
Restart=on-failure
RestartSec=5
LimitNOFILE=65536
LimitMEMLOCK=infinity

[Install]
WantedBy=multi-user.target
```

```bash
systemctl enable --now mcp-server
journalctl -u mcp-server -f
```
