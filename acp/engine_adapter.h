
#ifndef SLOP_ACP_ENGINE_ADAPTER_H_
#define SLOP_ACP_ENGINE_ADAPTER_H_

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "core/cancellation.h"
#include "core/database.h"
#include "nlohmann/json.hpp"

namespace slop::acp {

using SessionUpdateWriter = std::function<void(const nlohmann::json& update)>;

struct SessionPromptRequest {
  std::string session_id;
  std::string prompt;
};

struct SessionCancelRequest {
  std::string session_id;
};

using PromptExecutor =
    std::function<absl::StatusOr<std::string>(const std::string& session_id,
                                              const std::string& prompt,
                                              std::shared_ptr<slop::CancellationRequest> cancellation,
                                              const SessionUpdateWriter& session_update_writer)>;

absl::StatusOr<SessionPromptRequest> ParseSessionPromptParams(const nlohmann::json& params);
absl::StatusOr<SessionCancelRequest> ParseSessionCancelParams(const nlohmann::json& params);

absl::StatusOr<nlohmann::json> ExecuteSessionPrompt(Database* db, const SessionPromptRequest& request,
                                                     const PromptExecutor& executor,
                                                     const SessionUpdateWriter& session_update_writer,
                                                     std::shared_ptr<slop::CancellationRequest> cancellation);

inline nlohmann::json MakeSessionPromptResult(const std::string& session_id,
                                              const std::string& content) {
  return nlohmann::json({{"sessionId", session_id}, {"content", content}});
}

inline nlohmann::json MakePromptCompletionResult(std::string_view stop_reason = "end_turn") {
  return nlohmann::json({{"stopReason", std::string(stop_reason)}});
}

inline nlohmann::json MakeSessionPromptCancelledResult(const std::string& session_id) {
  return nlohmann::json({{"sessionId", session_id}, {"stopReason", "cancelled"}});
}

inline nlohmann::json MakeSessionCancelResult(const std::string& session_id) {
  return nlohmann::json({{"sessionId", session_id}, {"cancelled", true}});
}

}  // namespace slop::acp

#endif  // SLOP_ACP_ENGINE_ADAPTER_H_