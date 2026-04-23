
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

inline nlohmann::json MakeToolCallUpdateNotification(std::string_view session_id,
                                                     std::string_view tool_call_id,
                                                     std::string_view title,
                                                     std::string_view status,
                                                     std::optional<std::string_view> content_text = std::nullopt) {
  nlohmann::json update = {
      {"sessionUpdate", "tool_call_update"},
      {"toolCallId", std::string(tool_call_id)},
      {"title", std::string(title)},
      {"status", std::string(status)},
  };
  if (content_text.has_value()) {
    update["content"] = nlohmann::json::array({nlohmann::json({
        {"type", "text"},
        {"text", std::string(*content_text)},
    })});
  }
  return nlohmann::json({
      {"jsonrpc", "2.0"},
      {"method", "session/update"},
      {"params", nlohmann::json({{"sessionId", std::string(session_id)}, {"update", update}})},
  });
}

inline nlohmann::json MakeAgentMessageChunkNotification(std::string_view session_id, std::string_view content_text) {
  return MakeSessionUpdateNotification(session_id, SessionUpdateState::kCompleted, content_text);
}

}  // namespace slop::acp

#endif  // SLOP_ACP_UPDATE_PUBLISHER_H_