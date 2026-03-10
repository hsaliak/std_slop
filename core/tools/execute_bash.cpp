#include "core/tool_executor.h"

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "core/shell_util.h"
#include "core/status_macros.h"
#include "core/tools/common.h"
#include "core/json_utils.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandleExecuteBash(const nlohmann::json& args) const {
  RETURN_IF_ERROR(MaybeEnforceMailStagingGuard(mail_mode_));

  auto command = json_get<std::string>(args, "command");
  if (!command) {
    return absl::InvalidArgumentError("Invalid arguments: command is required and must be a string");
  }

  const bool allow_nonzero_exit = json_get_or<bool>(args, "allow_nonzero_exit", false);
  std::string command_to_run = *command;
  if (auto cwd = json_get<std::string>(args, "cwd"); cwd && !cwd->empty()) {
    command_to_run = absl::StrCat("cd ", EscapeShellArg(*cwd), " && ", *command);
  }

  ASSIGN_OR_RETURN(auto run_res, RunCommand(command_to_run));
  const std::string stdout_text = run_res.stdout_out;
  const std::string stderr_text = run_res.stderr_out;
  std::string out_text = stdout_text;
  if (!stderr_text.empty()) {
    if (!out_text.empty() && out_text.back() != '\n') out_text.push_back('\n');
    absl::StrAppend(&out_text, "\n### STDERR\n", stderr_text);
  }

  if (run_res.exit_code != 0 && !allow_nonzero_exit) {
    std::string msg = absl::StrCat("INTERNAL: Command failed with status ", run_res.exit_code,
                                   "\nCommand:\n", command_to_run);
    if (!stdout_text.empty()) {
      absl::StrAppend(&msg, "\n\nStdout:\n", stdout_text);
    }
    if (!stderr_text.empty()) {
      absl::StrAppend(&msg, "\n\nStderr:\n", stderr_text);
    }
    return absl::StrCat("Error: ", msg);
  }

  // Preserve top-level behavior as printable output while still exposing
  // structured fields to run_js callers (via JSON parse fallback).
  const nlohmann::json payload = {
      {"stdout", stdout_text},
      {"stderr", stderr_text},
      {"exit_code", run_res.exit_code},
      {"exitCode", run_res.exit_code},
      {"output", out_text},
      {"command", *command},
      {"executed_command", command_to_run},
      {"toString", out_text},
  };
  return payload.dump();
}

}  // namespace slop
