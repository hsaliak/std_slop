#include "mcp/local_tools/server.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"

#include "core/json_utils.h"
#include "mcp/types.h"

namespace slop::mcp::local_tools {
namespace {

mcp::ToolCallResult ToolResult(nlohmann::json structured_content, bool is_error = false) {
  mcp::ToolCallResult result;
  result.content.push_back({{"type", "text"}, {"text", json_dump(structured_content)}});
  result.structured_content = std::move(structured_content);
  result.is_error = is_error;
  return result;
}

mcp::ToolCallResult ToolError(const absl::Status& status) {
  return ToolResult({{"error", {{"message", std::string(status.message())}}}}, true);
}

mcp::server::ToolRegistration MakeTool(std::string name, std::string description, nlohmann::json input_schema,
                                       nlohmann::json annotations, mcp::server::ToolHandler handler) {
  mcp::server::ToolRegistration registration;
  registration.definition.name = std::move(name);
  registration.definition.description = std::move(description);
  registration.definition.input_schema = std::move(input_schema);
  registration.definition.annotations = std::move(annotations);
  registration.handler = std::move(handler);
  return registration;
}

nlohmann::json WorkspaceJson(const Workspace& workspace) {
  return {{"id", workspace.id},
          {"root", workspace.root.string()},
          {"is_default", workspace.is_default},
          {"is_git_worktree_root", workspace.is_git_worktree_root}};
}

}  // namespace

absl::StatusOr<mcp::server::Server> CreateServer(WorkspaceCatalog workspaces) {
  std::vector<mcp::server::ToolRegistration> tools;
  tools.push_back(MakeTool(
      "workspace_list", "List the launch workspace and currently available linked Git worktrees.",
      {{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
      {{"readOnlyHint", true}, {"openWorldHint", false}, {"destructiveHint", false}, {"idempotentHint", true}},
      [workspaces](const nlohmann::json&) mutable -> absl::StatusOr<mcp::ToolCallResult> {
        auto entries = workspaces.List();
        if (!entries.ok()) return ToolError(entries.status());

        nlohmann::json listed = nlohmann::json::array();
        for (const Workspace& workspace : *entries) listed.push_back(WorkspaceJson(workspace));
        return ToolResult({{"default", "launch"}, {"workspaces", std::move(listed)}});
      }));
  tools.push_back(MakeTool(
      "workspace_info", "Show the root of the selected workspace; defaults to the launch directory.",
      {{"type", "object"},
       {"properties", {{"workspace", {{"type", "string"}}}}},
       {"additionalProperties", false}},
      {{"readOnlyHint", true}, {"openWorldHint", false}, {"destructiveHint", false}, {"idempotentHint", true}},
      [workspaces](const nlohmann::json& arguments) mutable -> absl::StatusOr<mcp::ToolCallResult> {
        const std::string workspace_id = json_get_or(arguments, "workspace", std::string("launch"));
        auto workspace = workspaces.Resolve(workspace_id);
        if (!workspace.ok()) return ToolError(workspace.status());
        return ToolResult(WorkspaceJson(*workspace));
      }));

  mcp::ImplementationInfo identity;
  identity.name = "slop-local-tools";
  identity.version = "1.0";
  return mcp::server::Server::Create(std::move(identity), std::move(tools));
}

}  // namespace slop::mcp::local_tools
