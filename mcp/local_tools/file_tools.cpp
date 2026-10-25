#include "mcp/local_tools/file_tools.h"

#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "core/status_macros.h"
#include "mcp/local_tools/tool_utils.h"
#include "mcp/local_tools/workspace_file_system.h"
#include "mcp/types.h"

namespace slop::mcp::local_tools {
namespace {

nlohmann::json ObjectSchema(nlohmann::json properties, std::vector<std::string> required = {}) {
  nlohmann::json schema = {{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}};
  if (!required.empty()) schema["required"] = std::move(required);
  return schema;
}

absl::StatusOr<std::string> RequiredString(const nlohmann::json& arguments, const std::string& key,
                                           bool allow_empty = false) {
  const auto value = json_get<std::string>(arguments, key);
  if (!value) {
    return absl::InvalidArgumentError(
        json_at(arguments, key) == nullptr ? absl::StrCat("Missing mandatory field: ", key)
                                           : absl::StrCat(key, " must be a string"));
  }
  if (!allow_empty && value->empty()) return absl::InvalidArgumentError(absl::StrCat(key, " must be non-empty"));
  return *value;
}

absl::StatusOr<int> IntegerValue(const nlohmann::json& arguments, const std::string& key, int minimum, int maximum) {
  const auto parsed = json_get<int>(arguments, key);
  if (!parsed) return absl::InvalidArgumentError(absl::StrCat(key, " must be an integer"));
  if (*parsed < minimum || *parsed > maximum) {
    return absl::InvalidArgumentError(absl::StrCat(key, " must be between ", minimum, " and ", maximum));
  }
  return *parsed;
}

absl::StatusOr<int> OptionalInteger(const nlohmann::json& arguments, const std::string& key, int default_value,
                                    int minimum, int maximum) {
  const nlohmann::json* value = json_at(arguments, key);
  if (value == nullptr) return default_value;
  return IntegerValue(arguments, key, minimum, maximum);
}

absl::StatusOr<std::optional<int>> OptionalLine(const nlohmann::json& arguments, const std::string& key) {
  const nlohmann::json* value = json_at(arguments, key);
  if (value == nullptr) return std::nullopt;
  ASSIGN_OR_RETURN(const int line, IntegerValue(arguments, key, 1, std::numeric_limits<int>::max()));
  return std::optional<int>(line);
}

absl::StatusOr<std::vector<std::string>> IgnorePatterns(const nlohmann::json& arguments) {
  const nlohmann::json* value = json_at(arguments, "ignore");
  if (value == nullptr) return std::vector<std::string>{};
  auto parsed = json_get<std::vector<std::string>>(arguments, "ignore");
  if (!parsed) return absl::InvalidArgumentError("ignore must be an array of strings");
  if (parsed->size() > 100) return absl::ResourceExhaustedError("ignore may contain at most 100 patterns");
  for (const std::string& pattern : *parsed) {
    if (pattern.find('\0') != std::string::npos) {
      return absl::InvalidArgumentError("ignore patterns must not contain NUL bytes");
    }
  }
  return *parsed;
}

absl::Status ValidateObject(const nlohmann::json& arguments) {
  if (!json_is<nlohmann::json::object_t>(arguments)) {
    return absl::InvalidArgumentError("Tool arguments must be a JSON object");
  }
  return absl::OkStatus();
}

nlohmann::json StringSchema() { return {{"type", "string"}}; }

nlohmann::json IgnoreSchema() {
  return {{"type", "array"}, {"items", {{"type", "string"}}}};
}

mcp::server::ToolRegistration MakeFileTool(std::string name, std::string description, nlohmann::json schema,
                                           bool read_only, mcp::server::ToolHandler handler) {
  return mcp::server::MakeToolRegistration(
      std::move(name), std::move(description), std::move(schema),
      {{"readOnlyHint", read_only}, {"openWorldHint", false}, {"destructiveHint", !read_only},
       {"idempotentHint", read_only}},
      std::move(handler));
}

}  // namespace

std::vector<mcp::server::ToolRegistration> CreateFileTools(WorkspaceCatalog workspaces) {
  std::vector<mcp::server::ToolRegistration> tools;

  nlohmann::json read_properties = {{"path", StringSchema()}, {"workspace", StringSchema()}};
  read_properties["start_line"] = {
      {"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<int>::max()}};
  read_properties["end_line"] = {
      {"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<int>::max()}};
  read_properties["line_numbers"] = {{"type", "boolean"}};
  tools.push_back(MakeFileTool(
      "read_file", "Read a workspace-relative file with optional line range and line numbers.",
      ObjectSchema(std::move(read_properties), {"path"}), true,
      [workspaces](const nlohmann::json& arguments) -> absl::StatusOr<mcp::ToolCallResult> {
        const absl::Status object_status = ValidateObject(arguments);
        if (!object_status.ok()) return MakeToolError(object_status);
        auto path_or = RequiredString(arguments, "path");
        if (!path_or.ok()) return MakeToolError(path_or.status());
        auto start_line_or = OptionalLine(arguments, "start_line");
        if (!start_line_or.ok()) return MakeToolError(start_line_or.status());
        auto end_line_or = OptionalLine(arguments, "end_line");
        if (!end_line_or.ok()) return MakeToolError(end_line_or.status());
        auto line_numbers_or = OptionalToolBool(arguments, "line_numbers", false);
        if (!line_numbers_or.ok()) return MakeToolError(line_numbers_or.status());
        auto workspace_or = ResolveWorkspace(workspaces, arguments);
        if (!workspace_or.ok()) return MakeToolError(workspace_or.status());
        const std::string path = std::move(*path_or);
        const std::optional<int> start_line = *start_line_or;
        const std::optional<int> end_line = *end_line_or;
        const bool line_numbers = *line_numbers_or;
        const Workspace workspace = std::move(*workspace_or);
        auto content = ReadWorkspaceFile(workspace.root, path, {start_line, end_line, line_numbers});
        if (!content.ok()) return MakeToolError(content.status());
        return MakeToolResult(*content, {{"workspace", workspace.id}, {"path", path}, {"content", *content}});
      }));

  nlohmann::json directory_properties = {{"path", StringSchema()}, {"workspace", StringSchema()},
                                         {"ignore", IgnoreSchema()}};
  directory_properties["depth"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 8}};
  directory_properties["include_ignored"] = {{"type", "boolean"}};
  tools.push_back(MakeFileTool(
      "list_directory", "List files and directories inside the selected workspace.",
      ObjectSchema(std::move(directory_properties)), true,
      [workspaces](const nlohmann::json& arguments) -> absl::StatusOr<mcp::ToolCallResult> {
        const absl::Status object_status = ValidateObject(arguments);
        if (!object_status.ok()) return MakeToolError(object_status);
        auto path_or = OptionalToolString(arguments, "path", ".");
        if (!path_or.ok()) return MakeToolError(path_or.status());
        auto depth_or = OptionalInteger(arguments, "depth", 1, 1, 8);
        if (!depth_or.ok()) return MakeToolError(depth_or.status());
        auto include_ignored_or = OptionalToolBool(arguments, "include_ignored", false);
        if (!include_ignored_or.ok()) return MakeToolError(include_ignored_or.status());
        auto ignore_or = IgnorePatterns(arguments);
        if (!ignore_or.ok()) return MakeToolError(ignore_or.status());
        auto workspace_or = ResolveWorkspace(workspaces, arguments);
        if (!workspace_or.ok()) return MakeToolError(workspace_or.status());
        const std::string path = std::move(*path_or);
        const int depth = *depth_or;
        const bool include_ignored = *include_ignored_or;
        const std::vector<std::string> ignore = std::move(*ignore_or);
        const Workspace workspace = std::move(*workspace_or);
        auto output = ListWorkspaceDirectory(workspace.root, path, depth, include_ignored, ignore);
        if (!output.ok()) return MakeToolError(output.status());
        return MakeToolResult(*output,
                              {{"workspace", workspace.id}, {"path", path}, {"entries", *output}});
      }));

  nlohmann::json grep_properties = {{"pattern", StringSchema()}, {"path", StringSchema()},
                                    {"workspace", StringSchema()}, {"ignore", IgnoreSchema()}};
  grep_properties["context"] = {{"type", "integer"}, {"minimum", 0}, {"maximum", 20}};
  grep_properties["limit"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}};
  grep_properties["fixed_strings"] = {{"type", "boolean"}};
  grep_properties["include_ignored"] = {{"type", "boolean"}};
  tools.push_back(MakeFileTool(
      "grep", "Search workspace text files with literal or RE2 regular-expression matching.",
      ObjectSchema(std::move(grep_properties), {"pattern"}), true,
      [workspaces](const nlohmann::json& arguments) -> absl::StatusOr<mcp::ToolCallResult> {
        const absl::Status object_status = ValidateObject(arguments);
        if (!object_status.ok()) return MakeToolError(object_status);
        auto pattern_or = RequiredString(arguments, "pattern");
        if (!pattern_or.ok()) return MakeToolError(pattern_or.status());
        auto path_or = OptionalToolString(arguments, "path", ".");
        if (!path_or.ok()) return MakeToolError(path_or.status());
        auto context_or = OptionalInteger(arguments, "context", 0, 0, 20);
        if (!context_or.ok()) return MakeToolError(context_or.status());
        auto limit_or = OptionalInteger(arguments, "limit", 200, 1, 1000);
        if (!limit_or.ok()) return MakeToolError(limit_or.status());
        auto fixed_strings_or = OptionalToolBool(arguments, "fixed_strings", false);
        if (!fixed_strings_or.ok()) return MakeToolError(fixed_strings_or.status());
        auto include_ignored_or = OptionalToolBool(arguments, "include_ignored", false);
        if (!include_ignored_or.ok()) return MakeToolError(include_ignored_or.status());
        auto ignore_or = IgnorePatterns(arguments);
        if (!ignore_or.ok()) return MakeToolError(ignore_or.status());
        auto workspace_or = ResolveWorkspace(workspaces, arguments);
        if (!workspace_or.ok()) return MakeToolError(workspace_or.status());
        const std::string pattern = std::move(*pattern_or);
        const std::string path = std::move(*path_or);
        const int context = *context_or;
        const int limit = *limit_or;
        const bool fixed_strings = *fixed_strings_or;
        const bool include_ignored = *include_ignored_or;
        const std::vector<std::string> ignore = std::move(*ignore_or);
        const Workspace workspace = std::move(*workspace_or);
        auto output = GrepWorkspaceFiles(workspace.root, path, pattern, context, limit, fixed_strings, include_ignored,
                                         ignore);
        if (!output.ok()) return MakeToolError(output.status());
        return MakeToolResult(*output, {{"workspace", workspace.id}, {"path", path}, {"matches", *output}});
      }));

  nlohmann::json write_properties = {
      {"path", StringSchema()}, {"content", StringSchema()}, {"workspace", StringSchema()}};
  tools.push_back(MakeFileTool(
      "write_file", "Atomically write a file inside the selected workspace.",
      ObjectSchema(std::move(write_properties), {"path", "content"}), false,
      [workspaces](const nlohmann::json& arguments) -> absl::StatusOr<mcp::ToolCallResult> {
        const absl::Status object_status = ValidateObject(arguments);
        if (!object_status.ok()) return MakeToolError(object_status);
        auto path_or = RequiredString(arguments, "path");
        if (!path_or.ok()) return MakeToolError(path_or.status());
        auto content_or = RequiredString(arguments, "content", true);
        if (!content_or.ok()) return MakeToolError(content_or.status());
        auto workspace_or = ResolveWorkspace(workspaces, arguments);
        if (!workspace_or.ok()) return MakeToolError(workspace_or.status());
        const std::string path = std::move(*path_or);
        const std::string content = std::move(*content_or);
        const Workspace workspace = std::move(*workspace_or);
        auto output = WriteWorkspaceFile(workspace.root, path, content);
        if (!output.ok()) return MakeToolError(output.status());
        return MakeToolResult(*output,
                              {{"workspace", workspace.id}, {"path", path}, {"bytes_written", content.size()}});
      }));

  nlohmann::json edit_properties = {{"path", StringSchema()}, {"workspace", StringSchema()},
                                    {"edits", {{"type", "array"}, {"items", {{"type", "object"}}}}}};
  tools.push_back(MakeFileTool(
      "edit_tool", "Apply exact-text edits to a workspace file and replace it atomically.",
      ObjectSchema(std::move(edit_properties), {"path", "edits"}), false,
      [workspaces](const nlohmann::json& arguments) -> absl::StatusOr<mcp::ToolCallResult> {
        const absl::Status object_status = ValidateObject(arguments);
        if (!object_status.ok()) return MakeToolError(object_status);
        auto path_or = RequiredString(arguments, "path");
        if (!path_or.ok()) return MakeToolError(path_or.status());
        const nlohmann::json* edits = json_at(arguments, "edits");
        if (edits == nullptr) return MakeToolError(absl::InvalidArgumentError("Missing mandatory field: edits"));
        auto workspace_or = ResolveWorkspace(workspaces, arguments);
        if (!workspace_or.ok()) return MakeToolError(workspace_or.status());
        const std::string path = std::move(*path_or);
        const Workspace workspace = std::move(*workspace_or);
        auto result = EditWorkspaceFile(workspace.root, path, *edits);
        if (!result.ok()) return MakeToolError(result.status());
        nlohmann::json structured = {{"workspace", workspace.id},
                                     {"path", path},
                                     {"edits", result->edit_count},
                                     {"bytes_before", result->bytes_before},
                                     {"bytes_after", result->content.size()},
                                     {"changes", std::move(result->changes)}};
        return MakeJsonToolResult(std::move(structured));
      }));

  return tools;
}

}  // namespace slop::mcp::local_tools
