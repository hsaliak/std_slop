
#ifndef SLOP_ACP_SESSION_STORE_H_
#define SLOP_ACP_SESSION_STORE_H_

#include <optional>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "core/database.h"
#include "nlohmann/json.hpp"

namespace slop::acp {

struct SessionNewRequest {
  std::optional<std::string> session_id;
};

absl::StatusOr<SessionNewRequest> ParseSessionNewParams(const nlohmann::json& params);

bool IsValidSessionId(std::string_view session_id);

absl::StatusOr<std::string> CreateSession(Database* db, const SessionNewRequest& request);

inline nlohmann::json MakeSessionNewResult(std::string_view session_id) {
  return nlohmann::json({
      {"sessionId", std::string(session_id)},
  });
}

}  // namespace slop::acp

#endif  // SLOP_ACP_SESSION_STORE_H_