
#ifndef SLOP_ACP_UPDATE_PUBLISHER_H_
#define SLOP_ACP_UPDATE_PUBLISHER_H_

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
                                                    SessionUpdateState state) {
  return nlohmann::json({
      {"jsonrpc", "2.0"},
      {"method", "session/update"},
      {"params",
       nlohmann::json({
           {"sessionId", std::string(session_id)},
           {"state", std::string(SessionUpdateStateToString(state))},
       })},
  });
}

}  // namespace slop::acp

#endif  // SLOP_ACP_UPDATE_PUBLISHER_H_