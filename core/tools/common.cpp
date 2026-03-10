#include "core/tools/common.h"

#include <cstdlib>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "core/database.h"
#include "core/shell_util.h"
#include "core/status_macros.h"
#include "core/json_utils.h"

namespace slop {

bool IsDebugToolsEnabled() { return std::getenv("SLOP_DEBUG_TOOLS") != nullptr; }

std::string JsonKeys(const nlohmann::json& j) {
  if (!j.is_object()) return "<non-object>";
  std::vector<std::string> keys;
  keys.reserve(j.size());
  for (auto it = j.begin(); it != j.end(); ++it) {
    keys.push_back(it.key());
  }
  return keys.empty() ? "<empty>" : absl::StrJoin(keys, ",");
}

std::string TruncateForLog(const std::string& s, size_t max_len) {
  if (s.size() <= max_len) return s;
  return s.substr(0, max_len) + "...";
}

absl::StatusOr<std::optional<int>> ParseOptionalInteger(const nlohmann::json& args, const std::string& key) {
  const auto* v = json_at(args, key);
  if (!v || v->is_null()) return std::optional<int>();
  if (v->is_number_integer()) return v->get<int>();
  if (v->is_string()) {
    int parsed = 0;
    if (absl::SimpleAtoi(v->get<std::string>(), &parsed)) {
      return parsed;
    }
  }
  return absl::InvalidArgumentError(absl::StrCat(key, " must be an integer"));
}

std::string TrimNewlines(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

absl::StatusOr<std::string> GetCurrentBranchName() {
  const char* forced = std::getenv("SLOP_FORCE_BRANCH_NAME");
  if (forced && *forced) {
    return std::string(forced);
  }
  ASSIGN_OR_RETURN(auto branch_res, RunCommand("git rev-parse --abbrev-ref HEAD"));
  return TrimNewlines(branch_res.stdout_out);
}

absl::Status MaybeEnforceMailStagingGuard(bool mail_mode) {
  if (!mail_mode) return absl::OkStatus();
  const char* skip = std::getenv("SLOP_SKIP_STAGING_CHECK");
  if (skip && *skip && std::string(skip) != "0") {
    return absl::OkStatus();
  }
  ASSIGN_OR_RETURN(const std::string branch, GetCurrentBranchName());
  if (!absl::StartsWith(branch, "slop/staging/")) {
    return absl::FailedPreconditionError(
        absl::StrCat("Destructive operations are only allowed on 'slop/staging/*' branches. Current branch: ",
                     branch));
  }
  return absl::OkStatus();
}

absl::StatusOr<nlohmann::json> ParseDbRows(const std::string& rows_json, const std::string& context) {
  auto rows = json_parse(rows_json);
  if (!rows.has_value() || !rows->is_array()) {
    return absl::InternalError(absl::StrCat("Failed to parse DB rows for ", context));
  }
  return *rows;
}

absl::StatusOr<std::string> ResolveBaseBranch(Database* db, const std::string& requested_base) {
  if (!requested_base.empty()) return requested_base;
  ASSIGN_OR_RETURN(const std::string current, GetCurrentBranchName());
  if (!db) return std::string("main");

  ASSIGN_OR_RETURN(auto rows_json,
                   db->Query("SELECT base_branch FROM staging_branches WHERE branch_name = ? LIMIT 1", {current}));
  ASSIGN_OR_RETURN(auto rows, ParseDbRows(rows_json, "staging branch base lookup"));
  if (!rows.empty() && rows[0].is_object()) {
    std::string base = rows[0].value("base_branch", "");
    if (!base.empty()) {
      return base;
    }
  }

  if (absl::StartsWith(current, "slop/staging/")) {
    return absl::InternalError(
        absl::StrCat("Base branch not found in database for staging branch '", current, "'."));
  }
  return std::string("main");
}

absl::Status AssertCleanWorkspace() {
  ASSIGN_OR_RETURN(auto res, RunCommand("git status --porcelain"));
  if (!res.stdout_out.empty()) {
    return absl::FailedPreconditionError(
        "Working tree is dirty. Please commit, stash, or discard changes before finalizing.");
  }
  return absl::OkStatus();
}

std::string CanonicalStagingBranch(const std::string& branch) {
  std::string out = branch;
  const std::string kPrefix = "slop/staging/";
  while (absl::StartsWith(out, kPrefix)) {
    out = out.substr(kPrefix.size());
  }
  return absl::StrCat(kPrefix, out);
}

}  // namespace slop





