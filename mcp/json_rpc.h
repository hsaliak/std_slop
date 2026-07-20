#ifndef SLOP_MCP_JSON_RPC_H_
#define SLOP_MCP_JSON_RPC_H_

#include <string>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "mcp/types.h"
#include "nlohmann/json.hpp"

namespace slop::mcp {

nlohmann::json BuildJsonRpcRequest(const JsonRpcId& id, absl::string_view method,
                                   const nlohmann::json& params = nullptr);
nlohmann::json BuildJsonRpcNotification(absl::string_view method, const nlohmann::json& params = nullptr);

absl::StatusOr<JsonRpcResponse> ParseJsonRpcResponse(const nlohmann::json& message);
absl::StatusOr<nlohmann::json> ParseJsonRpcMessage(absl::string_view raw);

std::string JsonRpcIdToString(const JsonRpcId& id);

}  // namespace slop::mcp

#endif  // SLOP_MCP_JSON_RPC_H_
