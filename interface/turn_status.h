#ifndef SLOP_INTERFACE_TURN_STATUS_H_
#define SLOP_INTERFACE_TURN_STATUS_H_

#include <optional>
#include <string>

#include "absl/time/time.h"

#include "core/responses_types.h"

namespace slop {

enum class TurnPhase {
  kPreparing,
  kConnecting,
  kWaitingForModel,
  kReceiving,
  kRunningTools,
  kWaitingForFollowUp,
  kCompleted,
  kFailed,
  kCancelled,
};

struct TurnStatus {
  TurnPhase phase = TurnPhase::kPreparing;
  std::string detail;
  int received_tokens = 0;
  std::optional<ResponseUsage> usage;
  absl::Duration elapsed = absl::ZeroDuration();
};

}  // namespace slop

#endif  // SLOP_INTERFACE_TURN_STATUS_H_
