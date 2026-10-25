#include "mcp/local_tools/shell_tool.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "core/shell_util.h"
#include "mcp/local_tools/tool_utils.h"
#include "mcp/local_tools/workspace_file_system.h"
#include "mcp/types.h"

namespace slop::mcp::local_tools {
namespace {

constexpr int kDefaultTimeoutSeconds = 30;
constexpr int kMaxTimeoutSeconds = 45;
constexpr std::size_t kMaxCommandBytes = 64 * 1024;
constexpr std::size_t kMaxOutputBytes = 1024 * 1024;

absl::StatusOr<std::string> RequiredCommand(const nlohmann::json& arguments) {
  auto command = json_get<std::string>(arguments, "command");
  if (!command) {
    return absl::InvalidArgumentError(json_at(arguments, "command") == nullptr ? "Missing mandatory field: command"
                                                                                : "command must be a string");
  }
  if (command->empty()) return absl::InvalidArgumentError("command must be non-empty");
  if (command->size() > kMaxCommandBytes) {
    return absl::ResourceExhaustedError(absl::StrCat("command exceeds the ", kMaxCommandBytes, " byte limit"));
  }
  if (command->find('\0') != std::string::npos) return absl::InvalidArgumentError("command contains a NUL byte");
  return *command;
}

absl::StatusOr<int> OptionalTimeout(const nlohmann::json& arguments) {
  const nlohmann::json* value = json_at(arguments, "timeout_seconds");
  if (value == nullptr) return kDefaultTimeoutSeconds;
  auto timeout = json_get<int>(arguments, "timeout_seconds");
  if (!timeout) return absl::InvalidArgumentError("timeout_seconds must be an integer");
  if (*timeout < 1 || *timeout > kMaxTimeoutSeconds) {
    return absl::InvalidArgumentError(absl::StrCat("timeout_seconds must be between 1 and ", kMaxTimeoutSeconds));
  }
  return *timeout;
}

nlohmann::json ShellToolSchema() {
  return {{"type", "object"},
          {"properties",
           {{"command", {{"type", "string"}, {"minLength", 1}, {"maxLength", kMaxCommandBytes}}},
            {"workspace", {{"type", "string"}}},
            {"cwd", {{"type", "string"}}},
            {"timeout_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxTimeoutSeconds}}},
            {"allow_nonzero_exit", {{"type", "boolean"}}}}},
          {"required", {"command"}},
          {"additionalProperties", false}};
}

mcp::ToolCallResult ShellFailure(std::string message, const std::string& command, const std::string& workspace,
                                 const std::string& cwd, int timeout_seconds) {
  return MakeJsonToolResult({{"error", {{"message", std::move(message)}}},
                             {"command", command},
                             {"workspace", workspace},
                             {"cwd", cwd},
                             {"timeout_seconds", timeout_seconds}},
                            true);
}

}  // namespace

std::vector<mcp::server::ToolRegistration> CreateShellTools(WorkspaceCatalog workspaces) {
  mcp::server::ToolRegistration registration = mcp::server::MakeToolRegistration(
      "execute_bash", "Run a shell command with the selected workspace as its initial working directory. The shell is "
                      "not sandboxed and may access paths outside the workspace.",
      ShellToolSchema(),
      {{"readOnlyHint", false}, {"openWorldHint", true}, {"destructiveHint", true}, {"idempotentHint", false}},
      [workspaces](const nlohmann::json& arguments) -> absl::StatusOr<mcp::ToolCallResult> {
        if (!json_is<nlohmann::json::object_t>(arguments)) {
          return MakeToolError(absl::InvalidArgumentError("Tool arguments must be a JSON object"));
        }
        auto command_or = RequiredCommand(arguments);
        if (!command_or.ok()) return MakeToolError(command_or.status());
        const std::string command = std::move(*command_or);
        auto cwd_or = OptionalToolString(arguments, "cwd", ".");
        if (!cwd_or.ok()) return MakeToolError(cwd_or.status());
        const std::string relative_cwd = std::move(*cwd_or);
        auto timeout_or = OptionalTimeout(arguments);
        if (!timeout_or.ok()) return MakeToolError(timeout_or.status());
        const int timeout_seconds = *timeout_or;
        auto allow_nonzero_or = OptionalToolBool(arguments, "allow_nonzero_exit", false);
        if (!allow_nonzero_or.ok()) return MakeToolError(allow_nonzero_or.status());
        const bool allow_nonzero_exit = *allow_nonzero_or;
        auto workspace_or = ResolveWorkspace(workspaces, arguments);
        if (!workspace_or.ok()) return MakeToolError(workspace_or.status());
        const Workspace workspace = std::move(*workspace_or);
        auto working_directory_or = ResolveWorkspaceDirectory(workspace.root, relative_cwd);
        if (!working_directory_or.ok()) return MakeToolError(working_directory_or.status());
        const std::filesystem::path working_directory = std::move(*working_directory_or);

        auto command_result = RunCommand(command, /*cancellation=*/nullptr, /*input=*/"", timeout_seconds,
                                         working_directory.string(), kMaxOutputBytes);
        if (!command_result.ok()) return ShellFailure(std::string(command_result.status().message()), command,
                                                      workspace.id, relative_cwd, timeout_seconds);

        std::string output = command_result->stdout_out;
        if (!command_result->stderr_out.empty()) {
          if (!output.empty() && output.back() != '\n') output.push_back('\n');
          absl::StrAppend(&output, "\n### STDERR\n", command_result->stderr_out);
        }
        const bool is_error = command_result->exit_code != 0 && !allow_nonzero_exit;
        nlohmann::json result = {{"command", command},
                                 {"workspace", workspace.id},
                                 {"cwd", relative_cwd},
                                 {"timeout_seconds", timeout_seconds},
                                 {"exit_code", command_result->exit_code},
                                 {"stdout", command_result->stdout_out},
                                 {"stderr", command_result->stderr_out},
                                 {"output", output}};
        if (is_error) {
          result["error"] = {{"message", absl::StrCat("Command failed with exit code ", command_result->exit_code)}};
        }
        return MakeToolResult(std::move(output), std::move(result), is_error);
      });
  return {std::move(registration)};
}

}  // namespace slop::mcp::local_tools
