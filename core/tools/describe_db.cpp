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
absl::StatusOr<std::string> ToolExecutor::HandleDescribeDb(const nlohmann::json& args) {
  (void)args;
  if (!db_) {
    return absl::FailedPreconditionError("Database not initialized");
  }
  // Keep output parity with JS implementation by returning raw query_db JSON.
  return db_->Query("SELECT name, sql FROM sqlite_master WHERE type='table'");
}

}  // namespace slop
