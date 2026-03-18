#include <vector>

#include "absl/status/status.h"

#include "core/json_utils.h"
#include "core/status_macros.h"
#include "tools/common.h"
#include "tools/tool_executor.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandleQueryDb(const nlohmann::json& args) {
  RETURN_IF_ERROR(ValidateQueryDbArgs(args));
  if (!db_) {
    return absl::FailedPreconditionError("Database not initialized");
  }

  auto sql = json_get<std::string>(args, "sql");
  CHECK(sql.has_value());

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
