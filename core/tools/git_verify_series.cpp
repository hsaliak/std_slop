
#include <sstream>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "core/shell_util.h"
#include "core/status_macros.h"
#include "core/tool_executor.h"
#include "core/tools/common.h"

namespace slop {

absl::StatusOr<std::string> ToolExecutor::HandleGitVerifySeries(
    const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
  RETURN_IF_ERROR(MaybeEnforceMailStagingGuard(mail_mode_));
  auto command = json_get<std::string>(args, "command");
  if (!command || command->empty()) {
    return absl::InvalidArgumentError("Missing required argument: command");
  }

  ASSIGN_OR_RETURN(const std::string base_branch,
                   ResolveBaseBranch(db_, json_get_or<std::string>(args, "base_branch", "")));
  ASSIGN_OR_RETURN(auto original_branch_res, RunCommand("git rev-parse --abbrev-ref HEAD", cancellation));
  const std::string original_branch = std::string(absl::StripAsciiWhitespace(original_branch_res.stdout_out));
  ASSIGN_OR_RETURN(auto original_head_res, RunCommand("git rev-parse HEAD", cancellation));
  const std::string original_head = std::string(absl::StripAsciiWhitespace(original_head_res.stdout_out));
  bool restore_detached = original_branch == "HEAD";

  absl::Cleanup restore_checkout = [&]() {
    const std::string restore_cmd = restore_detached
                                        ? absl::StrCat("git checkout --quiet --detach ", EscapeShellArg(original_head))
                                        : absl::StrCat("git checkout --quiet ", EscapeShellArg(original_branch));
    (void)RunCommand(restore_cmd, cancellation);
  };

  ASSIGN_OR_RETURN(auto commits_res,
                   RunCommand(absl::StrCat("git rev-list --reverse ", EscapeShellArg(base_branch), "..HEAD"),
                              cancellation));
  if (commits_res.exit_code != 0) {
    return absl::InternalError(
        absl::StrCat("Failed to enumerate commits: ", commits_res.stdout_out, commits_res.stderr_out));
  }

  std::vector<std::string> commits;
  std::stringstream ss(commits_res.stdout_out);
  std::string line;
  while (std::getline(ss, line)) {
    line = std::string(absl::StripAsciiWhitespace(line));
    if (!line.empty()) {
      commits.push_back(line);
    }
  }

  bool all_passed = true;
  nlohmann::json report = nlohmann::json::array();
  for (const auto& commit : commits) {
    ASSIGN_OR_RETURN(auto checkout_res,
                     RunCommand(absl::StrCat("git checkout --quiet ", EscapeShellArg(commit)), cancellation));
    if (checkout_res.exit_code != 0) {
      return absl::InternalError(
          absl::StrCat("Failed to checkout commit ", commit, ": ", checkout_res.stdout_out, checkout_res.stderr_out));
    }

    ASSIGN_OR_RETURN(auto cmd_res, RunCommand(*command, cancellation));
    const bool passed = cmd_res.exit_code == 0;
    all_passed = all_passed && passed;
    report.push_back({{"commit", commit},
                      {"command", *command},
                      {"status", passed ? "passed" : "failed"},
                      {"exit_code", cmd_res.exit_code},
                      {"stdout", cmd_res.stdout_out},
                      {"stderr", cmd_res.stderr_out}});
  }

  const std::string restore_cmd = restore_detached
                                      ? absl::StrCat("git checkout --quiet --detach ", EscapeShellArg(original_head))
                                      : absl::StrCat("git checkout --quiet ", EscapeShellArg(original_branch));
  ASSIGN_OR_RETURN(auto restore_res, RunCommand(restore_cmd, cancellation));
  if (restore_res.exit_code != 0) {
    return absl::InternalError(
        absl::StrCat("Series verified but failed to restore original checkout: ", restore_res.stdout_out,
                     restore_res.stderr_out));
  }
  std::move(restore_checkout).Cancel();

  return nlohmann::json({{"ok", true},
                         {"action", "verify_series"},
                         {"command", *command},
                         {"base_branch", base_branch},
                         {"all_passed", all_passed},
                         {"report", report}})
      .dump();
}

}  // namespace slop