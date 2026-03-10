#include "core/tool_executor.h"

#include <fstream>
#include <sstream>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"

#include "core/database.h"
#include "core/shell_util.h"
#include "core/status_macros.h"
#include "core/tool_dispatcher.h"
#include "core/tools/common.h"
#include "interface/color.h"
#include "interface/renderer.h"
#include "interface/terminal.h"
#include "core/json_utils.h"

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
  auto parsed_log = nlohmann::json::parse(log_res, nullptr, false);
  auto parsed_diff = nlohmann::json::parse(diff_res, nullptr, false);
  if (parsed_log.is_object() && parsed_log.contains("output") && parsed_log["output"].is_string()) {
    log_output = parsed_log["output"].get<std::string>();
  }
  if (parsed_diff.is_object() && parsed_diff.contains("output") && parsed_diff["output"].is_string()) {
    diff_output = parsed_diff["output"].get<std::string>();
  }

  return absl::StrCat("--- MAIL SERIES ---\nBase: ", base_branch, "\n\n", log_output, "\n\n--- FULL DIFF ---\n", diff_output);
}

}  // namespace slop
