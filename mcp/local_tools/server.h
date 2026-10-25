#ifndef SLOP_MCP_LOCAL_TOOLS_SERVER_H_
#define SLOP_MCP_LOCAL_TOOLS_SERVER_H_

#include "absl/status/statusor.h"
#include "mcp/server/server.h"
#include "mcp/local_tools/workspace_catalog.h"

namespace slop::mcp::local_tools {

absl::StatusOr<mcp::server::Server> CreateServer(WorkspaceCatalog workspaces);

}  // namespace slop::mcp::local_tools

#endif  // SLOP_MCP_LOCAL_TOOLS_SERVER_H_
