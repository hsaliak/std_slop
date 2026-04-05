
#include "acp/engine_adapter.h"

#include <string_view>

#include "acp/session_store.h"
#include "absl/status/status.h"
#include "core/json_utils.h"
#include "core/status_macros.h"

namespace slop::acp {
namespace {

absl::StatusOr<bool> SessionExists(Database* db, std::string_view session_id) {
  ASSIGN_OR_RETURN(auto stmt, db->Prepare("SELECT 1 FROM sessions WHERE id = ? LIMIT 1"));
  RETURN_IF_ERROR(stmt->BindText(1, std::string(session_id)));
  return stmt->Step();
}

}  // namespace

absl::StatusOr<SessionPromptRequest> ParseSessionPromptParams(const nlohmann::json& params) {
  if (!params.is_object()) {
    return absl::InvalidArgumentError("session_prompt_params_must_be_object");
  }

  auto session_id = json_get<std::string>(params, "sessionId");
  if (!session_id.has_value() || session_id->empty()) {
    return absl::InvalidArgumentError("session_prompt_session_id_required");
  }
  if (!IsValidSessionId(*session_id)) {
    return absl::InvalidArgumentError("session_prompt_session_id_invalid");
  }

  auto prompt = json_get<std::string>(params, "prompt");
  if (!prompt.has_value()) {
    return absl::InvalidArgumentError("session_prompt_prompt_must_be_string");
  }
  if (prompt->empty()) {
    return absl::InvalidArgumentError("session_prompt_prompt_required");
  }

  SessionPromptRequest request;
  request.session_id = *session_id;
  request.prompt = *prompt;
  return request;
}

absl::StatusOr<nlohmann::json> ExecuteSessionPrompt(Database* db, const SessionPromptRequest& request,
                                                    const PromptExecutor& executor) {
  if (db == nullptr) {
    return absl::InvalidArgumentError("session_prompt_db_required");
  }

  ASSIGN_OR_RETURN(bool exists, SessionExists(db, request.session_id));
  if (!exists) {
    return absl::InvalidArgumentError("session_prompt_session_not_found");
  }

  if (!executor) {
    return absl::FailedPreconditionError("session_prompt_executor_required");
  }

  auto output_or = executor(request.session_id, request.prompt);
  if (!output_or.ok()) {
    if (output_or.status().code() == absl::StatusCode::kInvalidArgument) {
      return output_or.status();
    }
    return absl::InternalError("session_prompt_engine_failure");
  }

  return MakeSessionPromptResult(request.session_id, *output_or);
}

}  // namespace slop::acp