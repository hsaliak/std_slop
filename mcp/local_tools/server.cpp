#include "mcp/local_tools/server.h"

#include <string>
#include <utility>
#include <vector>

#include "core/json_utils.h"
#include "mcp/local_tools/file_tools.h"
#include "mcp/local_tools/tool_utils.h"
#include "mcp/types.h"

namespace slop::mcp::local_tools {
namespace {

nlohmann::json WorkspaceJson(const Workspace& workspace) {
  return {{"id", workspace.id},
          {"root", workspace.root.string()},
          {"is_default", workspace.is_default},
          {"is_git_worktree_root", workspace.is_git_worktree_root}};
}

}  // namespace

absl::StatusOr<mcp::server::Server> CreateServer(WorkspaceCatalog workspaces) {
  std::vector<mcp::server::ToolRegistration> tools;
  tools.push_back(MakeToolRegistration(
      "workspace_list", "List the launch workspace and currently available linked Git worktrees.",
      {{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
      {{"readOnlyHint", true}, {"openWorldHint", false}, {"destructiveHint", false}, {"idempotentHint", true}},
      [workspaces](const nlohmann::json&) -> absl::StatusOr<mcp::ToolCallResult> {
        auto entries = workspaces.List();
        if (!entries.ok()) return MakeToolError(entries.status());

        nlohmann::json listed = nlohmann::json::array();
        for (const Workspace& workspace : *entries) listed.push_back(WorkspaceJson(workspace));
        return MakeJsonToolResult({{"default", "launch"}, {"workspaces", std::move(listed)}});
      }));
  tools.push_back(MakeToolRegistration(
      "workspace_info", "Show the root of the selected workspace; defaults to the launch directory.",
      {{"type", "object"},
       {"properties", {{"workspace", {{"type", "string"}}}}},
       {"additionalProperties", false}},
      {{"readOnlyHint", true}, {"openWorldHint", false}, {"destructiveHint", false}, {"idempotentHint", true}},
      [workspaces](const nlohmann::json& arguments) -> absl::StatusOr<mcp::ToolCallResult> {
        auto workspace_id = json_get<std::string>(arguments, "workspace");
        if (json_at(arguments, "workspace") != nullptr && !workspace_id) {
          return MakeToolError(absl::InvalidArgumentError("workspace must be a string"));
        }
        auto workspace = workspaces.Resolve(workspace_id.value_or("launch"));
        if (!workspace.ok()) return MakeToolError(workspace.status());
        return MakeJsonToolResult(WorkspaceJson(*workspace));
      }));

  for (mcp::server::ToolRegistration& tool : CreateFileTools(workspaces)) {
    tools.push_back(std::move(tool));
  }

  mcp::ImplementationInfo identity;
  identity.name = "slop-local-tools";
  identity.version = "1.0";
  return mcp::server::Server::Create(std::move(identity), std::move(tools));
}

}  // namespace slop::mcp::local_tools
