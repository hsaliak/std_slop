
#include "acp/update_publisher.h"

namespace slop::acp {

std::string_view SessionUpdateStateToString(SessionUpdateState state) {
  switch (state) {
    case SessionUpdateState::kAccepted:
      return "accepted";
    case SessionUpdateState::kStarted:
      return "started";
    case SessionUpdateState::kExecutingTools:
      return "executing_tools";
    case SessionUpdateState::kCompleted:
      return "completed";
    case SessionUpdateState::kCancelled:
      return "cancelled";
  }
  return "accepted";
}

}  // namespace slop::acp
