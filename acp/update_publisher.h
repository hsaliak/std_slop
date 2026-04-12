
#ifndef SLOP_ACP_UPDATE_PUBLISHER_H_
#define SLOP_ACP_UPDATE_PUBLISHER_H_

#include <optional>
#include <string>
#include <string_view>

#include "nlohmann/json.hpp"

namespace slop::acp {

enum class SessionUpdateState {
  kAccepted,
  kStarted,
  kExecutingTools,
  kCompleted,
  kCancelled,
};

std::string_view SessionUpdateStateToString(SessionUpdateState state);

inline nlohmann::json MakeSessionUpdateNotification(std::string_view session_id,
                                                    SessionUpdateState state,
                                                    std::optional<std::string_view> content_text = std::nullopt) {
  std::string session_update;
  switch (state) {
    case SessionUpdateState::kAccepted:
    case SessionUpdateState::kStarted:
      session_update = "agent_thought_chunk";
      break;
    case SessionUpdateState::kExecutingTools:
      session_update = "tool_call_update";
      break;
    case SessionUpdateState::kCompleted:
      session_update = "agent_message_chunk";
      break;
    case SessionUpdateState::kCancelled:
      session_update = "agent_thought_chunk";
      break;
  }

  return nlohmann::json({
      {"jsonrpc", "2.0"},
      {"method", "session/update"},
      {"params",
       nlohmann::json({
           {"sessionId", std::string(session_id)},
           {"update",
            nlohmann::json({
                {"sessionUpdate", session_update},
                {"content",
                 nlohmann::json({
                     {"type", "text"},
                     {"text", std::string(content_text.value_or(SessionUpdateStateToString(state)))},
                 })},
            })},
       })},
  });
}

}  // namespace slop::acp

#endif  // SLOP_ACP_UPDATE_PUBLISHER_H_