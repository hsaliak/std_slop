#include "core/tool_executor.h"

#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"

#include "core/shell_util.h"
#include "core/status_macros.h"
#include "core/tools/common.h"
#include "core/json_utils.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandleGitFinalizeSeries(
    const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
  (void)cancellation;
  RETURN_IF_ERROR(MaybeEnforceMailStagingGuard(mail_mode_));
  RETURN_IF_ERROR(AssertCleanWorkspace());
  if (!db_) return absl::FailedPreconditionError("Database not initialized");

  ASSIGN_OR_RETURN(const std::string current_branch, GetCurrentBranchName());
  ASSIGN_OR_RETURN(const std::string target_branch,
                   ResolveBaseBranch(db_, json_get_or<std::string>(args, "target_branch", "")));
  ASSIGN_OR_RETURN(auto hash_res, RunCommand("git rev-parse HEAD"));
  const std::string hash = std::string(absl::StripAsciiWhitespace(hash_res.stdout_out));

  ASSIGN_OR_RETURN(auto approval_rows_json,
                   db_->Query("SELECT branch_name, approved_hash FROM patch_approvals WHERE approved_hash = ?", {hash}));
  ASSIGN_OR_RETURN(auto approval_rows, ParseDbRows(approval_rows_json, "approval lookup"));
  const std::string canonical_current = CanonicalStagingBranch(current_branch);
  bool approved = false;
  for (const auto& row : approval_rows) {
    if (!row.is_object()) continue;
    const std::string row_hash = row.value("approved_hash", "");
    const std::string row_branch = row.value("branch_name", "");
    if (row_hash == hash && (row_branch == current_branch || CanonicalStagingBranch(row_branch) == canonical_current)) {
      approved = true;
      break;
    }
  }

  ASSIGN_OR_RETURN(auto landed_res,
                   RunCommand(absl::StrCat("git merge-base --is-ancestor ", EscapeShellArg(hash), " ",
                                           EscapeShellArg(target_branch))));
  const bool already_landed = landed_res.exit_code == 0;
  if (!approved && !already_landed) {
    return absl::FailedPreconditionError(
        absl::StrCat("Patch series not approved or hash mismatch. Please obtain approval for hash ", hash,
                     " before finalizing."));
  }

  ASSIGN_OR_RETURN(auto checkout_res, RunCommand(absl::StrCat("git checkout ", EscapeShellArg(target_branch))));
  if (checkout_res.exit_code != 0) {
    return absl::InternalError(
        absl::StrCat("Failed to checkout target branch '", target_branch, "': ", checkout_res.stderr_out));
  }

  if (!already_landed) {
    ASSIGN_OR_RETURN(auto merge_res, RunCommand(absl::StrCat("git merge --ff-only ", EscapeShellArg(current_branch))));
    if (merge_res.exit_code != 0) {
      (void)RunCommand(absl::StrCat("git checkout ", EscapeShellArg(current_branch)));
      return absl::InternalError(absl::StrCat("Merge failed: ", merge_res.stderr_out));
    }
  }

  bool deleted_staging_branch = false;
  if (current_branch != target_branch) {
    ASSIGN_OR_RETURN(auto del_res, RunCommand(absl::StrCat("git branch -D ", EscapeShellArg(current_branch))));
    deleted_staging_branch = del_res.exit_code == 0;
  }

  RETURN_IF_ERROR(db_->Query("DELETE FROM staging_branches WHERE branch_name = ?", {current_branch}).status());
  RETURN_IF_ERROR(db_->Query("DELETE FROM staging_branches WHERE branch_name = ?", {canonical_current}).status());
  RETURN_IF_ERROR(db_->Query("DELETE FROM patch_approvals WHERE branch_name = ?", {current_branch}).status());
  RETURN_IF_ERROR(db_->Query("DELETE FROM patch_approvals WHERE branch_name = ?", {canonical_current}).status());
  RETURN_IF_ERROR(db_->Query("UPDATE settings SET mode = 'standard' WHERE id = 1", {}).status());
  mail_mode_ = false;

  ASSIGN_OR_RETURN(auto final_head_res, RunCommand("git rev-parse HEAD"));
  const std::string final_head = std::string(absl::StripAsciiWhitespace(final_head_res.stdout_out));

  return nlohmann::json({{"ok", true},
                         {"action", "finalize_series"},
                         {"mail_mode", "off"},
                         {"previous_branch", current_branch},
                         {"current_branch", target_branch},
                         {"head", final_head},
                         {"approved", approved},
                         {"already_landed", already_landed},
                         {"merged", !already_landed},
                         {"deleted_staging_branch", deleted_staging_branch},
                         {"cleaned_metadata", true},
                         {"notes",
                          already_landed
                              ? nlohmann::json::array({"Patch already landed on target branch", "Cleaned staging metadata",
                                                       "Mail mode disabled"})
                              : nlohmann::json::array({"Series finalized and merged", "Cleaned staging metadata",
                                                       "Mail mode disabled"})}})
      .dump();
}

}  // namespace slop
