#ifndef SLOP_MCP_DECISION_API_SERVER_H_
#define SLOP_MCP_DECISION_API_SERVER_H_

#include <memory>

#include "absl/status/statusor.h"

#include "mcp/decision_api/client.h"
#include "mcp/server/server.h"

namespace slop::mcp::decision_api {

absl::StatusOr<mcp::server::Server> CreateServer(std::shared_ptr<DecisionApiClient> client, const Config& config);

}  // namespace slop::mcp::decision_api

#endif  // SLOP_MCP_DECISION_API_SERVER_H_
