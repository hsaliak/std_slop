#ifndef SLOP_CORE_TOOLS_COMMON_H_
#define SLOP_CORE_TOOLS_COMMON_H_

#include <cstddef>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

namespace slop {

class Database;

bool IsDebugToolsEnabled();
std::string JsonKeys(const nlohmann::json& j);
std::string TruncateForLog(const std::string& s, size_t max_len = 240);
absl::StatusOr<std::optional<int>> ParseOptionalInteger(const nlohmann::json& args, const std::string& key);
std::string TrimNewlines(std::string s);
absl::StatusOr<std::string> GetCurrentBranchName();
absl::Status MaybeEnforceMailStagingGuard(bool mail_mode);
absl::StatusOr<nlohmann::json> ParseDbRows(const std::string& rows_json, const std::string& context);
absl::StatusOr<std::string> ResolveBaseBranch(Database* db, const std::string& requested_base);
absl::Status AssertCleanWorkspace();
std::string CanonicalStagingBranch(const std::string& branch);

}  // namespace slop

#endif  // SLOP_CORE_TOOLS_COMMON_H_


