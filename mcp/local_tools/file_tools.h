#ifndef SLOP_MCP_LOCAL_TOOLS_FILE_TOOLS_H_
#define SLOP_MCP_LOCAL_TOOLS_FILE_TOOLS_H_

#include <vector>

#include "mcp/local_tools/workspace_catalog.h"
#include "mcp/server/server.h"

namespace slop::mcp::local_tools {

std::vector<mcp::server::ToolRegistration> CreateFileTools(WorkspaceCatalog workspaces);

}  // namespace slop::mcp::local_tools

#endif  // SLOP_MCP_LOCAL_TOOLS_FILE_TOOLS_H_
