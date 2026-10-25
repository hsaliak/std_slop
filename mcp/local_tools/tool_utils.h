#ifndef SLOP_MCP_LOCAL_TOOLS_TOOL_UTILS_H_
#define SLOP_MCP_LOCAL_TOOLS_TOOL_UTILS_H_

#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "mcp/server/server.h"
#include "mcp/server/tool_utils.h"
#include "mcp/local_tools/workspace_catalog.h"
#include "mcp/types.h"

namespace slop::mcp::local_tools {

inline absl::StatusOr<Workspace> ResolveWorkspace(const WorkspaceCatalog& catalog,
                                                   const nlohmann::json& arguments) {
  const nlohmann::json* value = json_at(arguments, "workspace");
  if (value == nullptr) return catalog.Resolve("launch");
  auto workspace_id = json_getter<std::string>::get(*value);
  if (!workspace_id || workspace_id->empty()) {
    return absl::InvalidArgumentError("workspace must be a non-empty workspace ID string");
  }
  return catalog.Resolve(*workspace_id);
}

inline absl::StatusOr<std::string> OptionalToolString(const nlohmann::json& arguments, const std::string& key,
                                                      std::string default_value) {
  const nlohmann::json* value = json_at(arguments, key);
  if (value == nullptr) return default_value;
  auto parsed = json_getter<std::string>::get(*value);
  if (!parsed) return absl::InvalidArgumentError(absl::StrCat(key, " must be a string"));
  return *parsed;
}

inline absl::StatusOr<bool> OptionalToolBool(const nlohmann::json& arguments, const std::string& key,
                                             bool default_value) {
  const nlohmann::json* value = json_at(arguments, key);
  if (value == nullptr) return default_value;
  auto parsed = json_getter<bool>::get(*value);
  if (!parsed) return absl::InvalidArgumentError(absl::StrCat(key, " must be a boolean"));
  return *parsed;
}

inline mcp::ToolCallResult MakeJsonToolResult(nlohmann::json structured_content, bool is_error = false) {
  std::string text = json_dump(structured_content);
  return mcp::server::MakeToolCallResult(std::move(text), std::move(structured_content), is_error);
}

inline mcp::ToolCallResult MakeToolError(const absl::Status& status) {
  return MakeJsonToolResult({{"error", {{"message", std::string(status.message())}}}}, true);
}


}  // namespace slop::mcp::local_tools

#endif  // SLOP_MCP_LOCAL_TOOLS_TOOL_UTILS_H_
