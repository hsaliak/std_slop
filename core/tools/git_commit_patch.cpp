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
absl::StatusOr<std::string> ToolExecutor::HandleGitCommitPatch(
    const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
  (void)cancellation;
  RETURN_IF_ERROR(MaybeEnforceMailStagingGuard(mail_mode_));

  auto summary = json_get<std::string>(args, "summary");
  const std::string rationale = json_get_or<std::string>(args, "rationale", "");
  if (!summary || summary->empty()) {
    return absl::InvalidArgumentError("Summary is required");
  }
  if (summary->size() > 50) {
    return absl::InvalidArgumentError("Summary must be <= 50 characters");
  }

  const std::string full_msg = absl::StrCat(*summary, "\n\n", rationale);
  ASSIGN_OR_RETURN(auto commit_res, RunCommand(absl::StrCat("git commit -m ", EscapeShellArg(full_msg))));
  if (commit_res.exit_code != 0) {
    return absl::InternalError(absl::StrCat("Commit failed: ", commit_res.stdout_out, commit_res.stderr_out));
  }

  return HandleGitFormatPatchSeries(nlohmann::json::object(), cancellation);
}

}  // namespace slop
