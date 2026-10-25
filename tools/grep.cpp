#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"

#include "core/json_utils.h"
#include "core/status_macros.h"
#include "tools/common.h"
#include "tools/file_operations.h"
#include "tools/tool_executor.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandleGrep(const nlohmann::json& args) {
  auto pattern = json_get<std::string>(args, "pattern");
  if (!pattern || pattern->empty()) return absl::InvalidArgumentError("Missing mandatory field: pattern");

  const std::string search_path = json_get_or<std::string>(args, "path", ".");
  ASSIGN_OR_RETURN(const auto context_value, ParseOptionalInteger(args, "context"));
  ASSIGN_OR_RETURN(const auto limit_value, ParseOptionalInteger(args, "limit"));
  const int context = std::clamp(context_value.value_or(0), 0, 20);
  const int limit = std::clamp(limit_value.value_or(200), 1, 1000);
  const bool include_ignored = json_get_or<bool>(args, "include_ignored", false);
  const bool fixed_strings = json_get_or<bool>(args, "fixed_strings", false);

  std::vector<std::string> ignore_patterns;
  if (const nlohmann::json* ignore = json_at(args, "ignore"); ignore != nullptr && !ignore->is_null()) {
    if (const auto single = json_get<std::string>(args, "ignore")) {
      if (!single->empty()) ignore_patterns.push_back(*single);
    } else {
      auto parsed = json_get<std::vector<std::string>>(args, "ignore");
      if (!parsed) return absl::InvalidArgumentError("ignore must be a string or an array of strings");
      ignore_patterns = std::move(*parsed);
    }
  }

  return file_ops::GrepWorkspaceFiles(std::filesystem::path("."), search_path, *pattern, context, limit,
                                      fixed_strings, include_ignored, ignore_patterns);
}

}  // namespace slop
