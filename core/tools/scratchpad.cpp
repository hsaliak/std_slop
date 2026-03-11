#include "absl/status/status.h"

#include "core/json_utils.h"
#include "core/status_macros.h"
#include "core/tool_executor.h"

namespace slop {

absl::StatusOr<std::string> ToolExecutor::HandleReadScratchpad([[maybe_unused]] const nlohmann::json& args) {
  if (!db_) {
    return absl::FailedPreconditionError("Database unavailable");
  }
  if (session_id_.empty()) {
    return absl::FailedPreconditionError("No active session available for scratchpad operations.");
  }
  ASSIGN_OR_RETURN(auto content, db_->GetScratchpad(session_id_));
  return content;
}

absl::StatusOr<std::string> ToolExecutor::HandleWriteScratchpad(const nlohmann::json& args) {
  if (!db_) {
    return absl::FailedPreconditionError("Database unavailable");
  }
  if (session_id_.empty()) {
    return absl::FailedPreconditionError("No active session available for scratchpad operations.");
  }
  auto content = json_get<std::string>(args, "content");
  if (!content) {
    return absl::InvalidArgumentError("Missing mandatory field: content");
  }
  RETURN_IF_ERROR(db_->SetScratchpad(session_id_, *content));
  return "Scratchpad updated.";
}

}  // namespace slop
