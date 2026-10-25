#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/numbers.h"

#include "core/json_utils.h"
#include "tools/file_operations.h"
#include "tools/tool_executor.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandleListDirectory(const nlohmann::json& args) {
  const std::string path = json_get_or<std::string>(args, "path", ".");
  int depth = 1;
  if (auto depth_value = json_get<int>(args, "depth")) {
    depth = *depth_value;
  } else if (auto depth_string = json_get<std::string>(args, "depth")) {
    if (!absl::SimpleAtoi(*depth_string, &depth)) {
      return absl::InvalidArgumentError("depth must be an integer");
    }
  }

  const bool include_ignored = json_get_or<bool>(args, "include_ignored", false);
  std::vector<std::string> ignore_patterns;
  const nlohmann::json* ignore_json = json_at(args, "ignore");
  if (ignore_json != nullptr && !ignore_json->is_null()) {
    if (!ignore_json->is_array()) return absl::InvalidArgumentError("ignore must be an array of strings");
    auto parsed_patterns = json_get<std::vector<std::string>>(args, "ignore");
    if (!parsed_patterns) return absl::InvalidArgumentError("ignore must be an array of strings");
    ignore_patterns = std::move(*parsed_patterns);
  }

  return file_ops::ListWorkspaceDirectory(std::filesystem::path("."), path, depth, include_ignored,
                                          ignore_patterns);
}

}  // namespace slop
