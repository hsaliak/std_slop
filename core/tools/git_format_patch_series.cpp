#include "core/tool_executor.h"

#include "absl/strings/str_cat.h"
#include "core/shell_util.h"
#include "core/status_macros.h"
#include "core/json_utils.h"
#include "core/tools/common.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandleGitFormatPatchSeries(
    const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
  (void)cancellation;
  RETURN_IF_ERROR(MaybeEnforceMailStagingGuard(mail_mode_));
  ASSIGN_OR_RETURN(const std::string base_branch,
                   ResolveBaseBranch(db_, json_get_or<std::string>(args, "base_branch", "")));

  const std::string log_cmd =
      absl::StrCat("git log --reverse --format='### Patch [%n/%N]: %s ###%ncommit %H%nAuthor: %an <%ae>%nDate:   %ad%n%n    %s%n%n%b' ",
                   EscapeShellArg(base_branch), "..HEAD");
  ASSIGN_OR_RETURN(auto log_res, HandleExecuteBash({{"command", log_cmd}}));
  ASSIGN_OR_RETURN(auto diff_res,
                   HandleExecuteBash({{"command", absl::StrCat("git diff ", EscapeShellArg(base_branch), "..HEAD")}}));

  std::string log_output;
  std::string diff_output;
  const auto parsed_log = json_parse(log_res);
  const auto parsed_diff = json_parse(diff_res);
  if (parsed_log.has_value()) {
    log_output = json_get_or<std::string>(*parsed_log, "output", "");
  }
  if (parsed_diff.has_value()) {
    diff_output = json_get_or<std::string>(*parsed_diff, "output", "");
  }

  return absl::StrCat("--- MAIL SERIES ---\nBase: ", base_branch, "\n\n", log_output, "\n\n--- FULL DIFF ---\n", diff_output);
}

}  // namespace slop
