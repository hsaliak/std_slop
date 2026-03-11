#include "absl/status/status.h"

#include "core/tool_executor.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandleDescribeDb(const nlohmann::json& args) {
  (void)args;
  if (!db_) {
    return absl::FailedPreconditionError("Database not initialized");
  }
  // Keep output parity with JS implementation by returning raw query_db JSON.
  return db_->Query("SELECT name, sql FROM sqlite_master WHERE type='table'");
}

}  // namespace slop
