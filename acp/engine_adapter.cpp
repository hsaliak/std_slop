
#include "acp/engine_adapter.h"

#include <optional>
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

std::string ExtractTextFromPromptBlocks(const nlohmann::json& blocks) {
  if (!blocks.is_array()) {
    return "";
  }

  std::string text;
  for (const auto& block : blocks) {
    if (block.is_string()) {
      text += block.get<std::string>();
      continue;
    }
    if (!block.is_object()) {
      continue;
    }
    auto block_text = json_get<std::string>(block, "text");
    if (block_text.has_value()) {
      text += *block_text;
    }
  }
  return text;
}

std::optional<std::string> ExtractPromptText(const nlohmann::json& prompt_value) {
  if (prompt_value.is_string()) {
    return prompt_value.get<std::string>();
  }
  if (prompt_value.is_array()) {
    return ExtractTextFromPromptBlocks(prompt_value);
  }
  if (!prompt_value.is_object()) {
    return std::nullopt;
  }

  auto text = json_get<std::string>(prompt_value, "text");
  if (text.has_value()) {
    return *text;
  }

  const nlohmann::json content_blocks = json_get_or<nlohmann::json>(prompt_value, "content", nlohmann::json());
  if (content_blocks.is_array()) {
    return ExtractTextFromPromptBlocks(content_blocks);
  }

  const nlohmann::json blocks = json_get_or<nlohmann::json>(prompt_value, "blocks", nlohmann::json());
  if (blocks.is_array()) {
    return ExtractTextFromPromptBlocks(blocks);
  }

  return std::nullopt;
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

  const nlohmann::json prompt_value = json_get_or<nlohmann::json>(params, "prompt", nlohmann::json());
  auto prompt = ExtractPromptText(prompt_value);
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

absl::StatusOr<SessionCancelRequest> ParseSessionCancelParams(const nlohmann::json& params) {
  if (!params.is_object()) {
    return absl::InvalidArgumentError("session_cancel_params_must_be_object");
  }

  auto session_id = json_get<std::string>(params, "sessionId");
  if (!session_id.has_value() || session_id->empty()) {
    return absl::InvalidArgumentError("session_cancel_session_id_required");
  }
  if (!IsValidSessionId(*session_id)) {
    return absl::InvalidArgumentError("session_cancel_session_id_invalid");
  }

  SessionCancelRequest request;
  request.session_id = *session_id;
  return request;
}

absl::StatusOr<nlohmann::json> ExecuteSessionPrompt(Database* db, const SessionPromptRequest& request,
                                                    const PromptExecutor& executor,
                                                    std::shared_ptr<CancellationRequest> cancellation) {
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

  if (cancellation == nullptr) {
    cancellation = std::make_shared<CancellationRequest>();
  }

  auto output_or = executor(request.session_id, request.prompt, cancellation);
  if (!output_or.ok()) {
    if (output_or.status().code() == absl::StatusCode::kInvalidArgument) {
      return output_or.status();
    }
    if (output_or.status().code() == absl::StatusCode::kCancelled) {
      return MakeSessionPromptCancelledResult(request.session_id);
    }
    return absl::InternalError("session_prompt_engine_failure");
  }

  return MakeSessionPromptResult(request.session_id, *output_or);
}
}  // namespace slop::acp