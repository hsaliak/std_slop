#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "core/shell_util.h"
#include "core/status_macros.h"
#include "core/tool_executor.h"
#include "core/tools/common.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandleGitCreateStagingBranch(const nlohmann::json& args) {
  auto name = json_get<std::string>(args, "name");
  if (!name || name->empty()) {
    return absl::InvalidArgumentError("name is required");
  }
  std::string normalized = *name;
  while (absl::StartsWith(normalized, "slop/staging/")) {
    normalized = normalized.substr(std::string("slop/staging/").size());
  }
  if (normalized.empty()) {
    return absl::InvalidArgumentError("name must contain non-prefix characters");
  }
  const std::string staging_name = absl::StrCat("slop/staging/", normalized);
  std::string base_branch;
  if (auto requested_base = json_get<std::string>(args, "base_branch")) {
    base_branch = *requested_base;
  } else {
    ASSIGN_OR_RETURN(base_branch, GetCurrentBranchName());
  }

  auto res_or =
      RunCommand(absl::StrCat("git checkout -b ", EscapeShellArg(staging_name), " ", EscapeShellArg(base_branch)));
  if (!res_or.ok()) return res_or.status();
  auto res = *res_or;
  if (res.exit_code != 0 &&
      (absl::StrContains(res.stdout_out, "already exists") || absl::StrContains(res.stderr_out, "already exists"))) {
    ASSIGN_OR_RETURN(res, RunCommand(absl::StrCat("git checkout ", EscapeShellArg(staging_name))));
  }
  if (res.exit_code != 0) {
    return absl::InternalError(absl::StrCat("Failed to create staging branch: ", res.stdout_out, res.stderr_out));
  }

  if (db_) {
    RETURN_IF_ERROR(db_->Query("INSERT OR REPLACE INTO staging_branches (branch_name, parent_branch) VALUES (?, ?)",
                               {staging_name, base_branch})
                        .status());
  }

  return absl::StrCat("Created and checked out staging branch: ", staging_name, " (base: ", base_branch, ")");
}

}  // namespace slop
