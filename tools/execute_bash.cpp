#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "core/shell_util.h"
#include "core/status_macros.h"
#include "tools/tool_executor.h"
#include "tools/common.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandleExecuteBash(const nlohmann::json& args) const {
  RETURN_IF_ERROR(ValidateExecuteBashArgs(args));
  RETURN_IF_ERROR(MaybeEnforceMailStagingGuard(mail_mode_));
  constexpr int kDefaultTimeoutSeconds = 180;

  auto command = json_get<std::string>(args, "command");
  CHECK(command.has_value());

  const bool allow_nonzero_exit = json_get_or<bool>(args, "allow_nonzero_exit", false);
  const int timeout_seconds = json_get_or<int>(args, "timeout_seconds", kDefaultTimeoutSeconds);

  std::string command_to_run = *command;
  if (auto cwd = json_get<std::string>(args, "cwd"); cwd && !cwd->empty()) {
    command_to_run = absl::StrCat("cd ", EscapeShellArg(*cwd), " && ", *command);
  }

  auto run_or = RunCommand(command_to_run, /*cancellation=*/nullptr, /*input=*/"", timeout_seconds);
  if (!run_or.ok()) {
    if (run_or.status().code() == absl::StatusCode::kDeadlineExceeded) {
      const nlohmann::json timeout_payload = {
          {"error", "Command timed out"},
          {"status", "DEADLINE_EXCEEDED"},
          {"command", *command},
          {"executed_command", command_to_run},
          {"timeout_seconds", timeout_seconds},
      };
      return absl::StrCat("Error: ", timeout_payload.dump());
    }
    return run_or.status();
  }

  const auto& run_res = *run_or;
  const std::string stdout_text = run_res.stdout_out;
  const std::string stderr_text = run_res.stderr_out;
  std::string out_text = stdout_text;
  if (!stderr_text.empty()) {
    if (!out_text.empty() && out_text.back() != '\n') out_text.push_back('\n');
    absl::StrAppend(&out_text, "\n### STDERR\n", stderr_text);
  }

  if (run_res.exit_code != 0 && !allow_nonzero_exit) {
    std::string msg =
        absl::StrCat("INTERNAL: Command failed with status ", run_res.exit_code, "\nCommand:\n", command_to_run);
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
      {"timeout_seconds", timeout_seconds},
      {"toString", out_text},
  };
  return payload.dump();
}

}  // namespace slop
