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
absl::StatusOr<std::string> ToolExecutor::HandleQueryDb(const nlohmann::json& args) {
  if (!db_) {
    return absl::FailedPreconditionError("Database not initialized");
  }
  if (!args.is_object()) {
    return absl::InvalidArgumentError("Arguments must be a JSON object");
  }

  auto sql = json_get<std::string>(args, "sql");
  if (!sql) {
    return absl::InvalidArgumentError("'sql' must be a string");
  }

  std::vector<std::string> params;
  if (auto p_array = json_get<nlohmann::json::array_t>(args, "params")) {
    for (const auto& p : *p_array) {
      if (p.is_string()) {
        params.push_back(p.get<std::string>());
      } else if (p.is_null()) {
        params.emplace_back("NULL");
      } else {
        // For numbers, booleans, objects, and arrays, stringify them.
        params.push_back(p.dump());
      }
    }
  }
  return db_->Query(*sql, params);
}

}  // namespace slop
