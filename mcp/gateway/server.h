#ifndef SLOP_MCP_GATEWAY_SERVER_H_
#define SLOP_MCP_GATEWAY_SERVER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "absl/status/statusor.h"

#include "js_runtime/runtime.h"
#include "mcp/gateway/broker.h"
#include "mcp/gateway/trace_logger.h"
#include "mcp/server/server.h"

namespace slop::mcp::gateway {

using RunJsExecutor = std::function<absl::StatusOr<nlohmann::json>(
    const std::string&, const nlohmann::json&, js_runtime::AsyncToolBroker*, js_runtime::RuntimeOptions,
    std::uint64_t)>;

absl::StatusOr<server::Server> CreateServer(std::shared_ptr<js_runtime::AsyncToolBroker> broker,
                                            js_runtime::RuntimeOptions options = {}, RunJsExecutor executor = {},
                                            std::shared_ptr<TraceLogger> trace_logger = nullptr);

}  // namespace slop::mcp::gateway

#endif  // SLOP_MCP_GATEWAY_SERVER_H_
