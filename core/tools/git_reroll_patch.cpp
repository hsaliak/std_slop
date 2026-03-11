#include <vector>

#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "core/shell_util.h"
#include "core/status_macros.h"
#include "core/tool_executor.h"
#include "core/tools/common.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandleGitRerollPatch(const nlohmann::json& args,
                                                               std::shared_ptr<CancellationRequest> cancellation) {
  RETURN_IF_ERROR(MaybeEnforceMailStagingGuard(mail_mode_));
  std::optional<int> idx;
  if (auto i = json_get<int>(args, "index")) {
    idx = *i;
  } else if (auto s = json_get<std::string>(args, "index")) {
    int parsed = 0;
    if (absl::SimpleAtoi(*s, &parsed)) idx = parsed;
  }
  ASSIGN_OR_RETURN(const std::string base_branch,
                   ResolveBaseBranch(db_, json_get_or<std::string>(args, "base_branch", "")));
  ASSIGN_OR_RETURN(const std::string current_branch, GetCurrentBranchName());
  RETURN_IF_ERROR(RequireNamedBranch(current_branch, "git_reroll_patch"));

  ASSIGN_OR_RETURN(auto head_before_res, RunCommand("git rev-parse HEAD"));
  const std::string head_before = std::string(absl::StripAsciiWhitespace(head_before_res.stdout_out));

  ASSIGN_OR_RETURN(auto log_res, HandleExecuteBash({{"command", absl::StrCat("git log --reverse --format=%H ",
                                                                             EscapeShellArg(base_branch), "..HEAD")}}));
  const auto parsed_log = json_parse(log_res);
  const std::string hashes = parsed_log.has_value() ? json_get_or<std::string>(*parsed_log, "stdout", "") : "";
  std::vector<std::string> commits;
  std::string token;
  for (char c : hashes) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!token.empty()) {
        commits.push_back(token);
        token.clear();
      }
    } else {
      token.push_back(c);
    }
  }
  if (!token.empty()) commits.push_back(token);

  const int index = idx.value_or(0);
  if (index < 1 || index > static_cast<int>(commits.size())) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid patch index ", index, " (total patches: ", commits.size(), ")"));
  }
  const std::string target_hash = commits[static_cast<size_t>(index - 1)];

  ASSIGN_OR_RETURN(auto fixup_res, RunCommand(absl::StrCat("git commit --fixup ", target_hash)));
  if (fixup_res.exit_code != 0) {
    return absl::InternalError("Failed to create fixup commit. Are there any changes staged?");
  }

  ASSIGN_OR_RETURN(auto rebase_res, RunCommand(absl::StrCat("GIT_SEQUENCE_EDITOR=true git rebase -i --autosquash ",
                                                            EscapeShellArg(base_branch))));
  if (rebase_res.exit_code != 0) {
    (void)RunCommand("git rebase --abort");
    return absl::InternalError(
        absl::StrCat("Rebase failed. You may have conflicts. Manual intervention required.\n", rebase_res.stderr_out));
  }

  ASSIGN_OR_RETURN(auto series, HandleGitFormatPatchSeries({{"base_branch", base_branch}}, cancellation));
  auto parsed_series = json_parse(series);
  if (!parsed_series || !parsed_series->is_object()) {
    return absl::StrCat("Successfully rerolled patch ", index, "\n\n", series);
  }

  ASSIGN_OR_RETURN(auto head_after_res, RunCommand("git rev-parse HEAD"));
  const std::string head_after = std::string(absl::StripAsciiWhitespace(head_after_res.stdout_out));

  nlohmann::json out = *parsed_series;
  out["action"] = "reroll_patch";
  out["status"] = "awaiting_review";
  out["rerolled_index"] = index;
  out["base_branch"] = base_branch;
  out["current_branch"] = current_branch;
  out["head_before"] = head_before;
  out["head"] = head_after;
  out["message"] = absl::StrCat("Successfully rerolled patch ", index, ". Awaiting review.");
  return out.dump();
}

}  // namespace slop
