
#ifndef SLOP_ACP_ENGINE_ADAPTER_H_
#define SLOP_ACP_ENGINE_ADAPTER_H_

#include <functional>
#include <string>

#include "absl/status/statusor.h"
#include "core/database.h"
#include "nlohmann/json.hpp"

namespace slop::acp {

struct SessionPromptRequest {
  std::string session_id;
  std::string prompt;
};

using PromptExecutor = std::function<absl::StatusOr<std::string>(const std::string& session_id,
                                                                  const std::string& prompt)>;

absl::StatusOr<SessionPromptRequest> ParseSessionPromptParams(const nlohmann::json& params);

absl::StatusOr<nlohmann::json> ExecuteSessionPrompt(Database* db, const SessionPromptRequest& request,
                                                    const PromptExecutor& executor);

inline nlohmann::json MakeSessionPromptResult(const std::string& session_id,
                                              const std::string& content) {
  return nlohmann::json({{"sessionId", session_id}, {"content", content}});
}

}  // namespace slop::acp

#endif  // SLOP_ACP_ENGINE_ADAPTER_H_