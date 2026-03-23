#pragma once
// seastar-mcp-sdk 公共入口
// 用户只需 #include <mcp/mcp.hh> 即可使用完整 SDK

#include "mcp/core/interfaces.hh"
#include "mcp/core/registry.hh"
#include "mcp/protocol/json_rpc.hh"
#include "mcp/router/dispatcher.hh"
#include "mcp/transport/transport.hh"
#include "mcp/server/mcp_shard.hh"
#include "mcp/server/mcp_server.hh"
#include "mcp/transport/stdio_transport.hh"
#include "mcp/transport/http_sse_transport.hh"
#include "mcp/transport/streamable_http_transport.hh"
#include "mcp/core/builder.hh"
