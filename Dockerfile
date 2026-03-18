# 必须使用 ubuntu:24.04，以匹配你本地的库版本 (ldd 显示的是 1.83.0 和 0.8)
FROM ubuntu:24.04

# 避免交互式提示
ENV DEBIAN_FRONTEND=noninteractive

# 只安装运行 MCP Server 绝对必须的动态链接运行库 (Runtime libs)
# 相比之前的 -dev 版本，这些包更小且专门用于运行
RUN apt-get update && apt-get install -y \
    libboost-program-options1.83.0 \
    libfmt9 \
    libgnutls30 \
    libyaml-cpp0.8 \
    libhwloc15 \
    liburing2 \
    libstdc++6 \
    libgcc-s1 \
    libc6 \
    libp11-kit0 \
    libidn2-0 \
    libunistring5 \
    libtasn1-6 \
    libnettle8 \
    libhogweed6 \
    libgmp10 \
    libudev1 \
    libffi8 \
    libcap2 \
    && rm -rf /var/lib/apt/lists/*

# 设置工作目录
WORKDIR /app

# 将你本地编译好的二进制文件拷贝进去
# 注意：构建时确保你在工程根目录，且 build/mcp_server 路径正确
COPY build/mcp_server /app/mcp_server

# 增加执行权限
RUN chmod +x /app/mcp_server

# 设置启动命令，严格限制资源，防止 Seastar 抢占导致容器或主机卡死
# -c 1: 使用 1 个核心 (对 MCP 服务足够了)
# -m 512M: 限制内存
# --default-log-level=warn: 减少日志干扰标准输出
ENTRYPOINT ["/app/mcp_server", "-c", "1", "-m", "512M", "--default-log-level=warn"]