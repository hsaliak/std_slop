#ifndef SLOP_CORE_RESPONSES_EVENT_DECODER_H_
#define SLOP_CORE_RESPONSES_EVENT_DECODER_H_

#include <string>
#include <optional>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "nlohmann/json.hpp"

namespace slop {

enum class ResponsesEventType {
  kTextDelta,
  kTextDone,
  kOutputItem,
  kCompleted,
  kFailed,
  kIncomplete,
  kUnknown,
};

struct ResponsesEvent {
  ResponsesEventType type = ResponsesEventType::kUnknown;
  nlohmann::json payload;
  std::string text;
};

// Incrementally frames Responses API server-sent events. Feed accepts arbitrary
// transport chunks; Finish flushes a final complete event without a blank line.
class ResponsesEventDecoder {
 public:
  absl::StatusOr<std::vector<ResponsesEvent>> Feed(absl::string_view chunk);
  absl::StatusOr<std::vector<ResponsesEvent>> Finish();
  static std::optional<nlohmann::json> NormalizeSsePayload(absl::string_view payload);

 private:
  absl::StatusOr<std::vector<ResponsesEvent>> ConsumeLines(bool finish);
  absl::StatusOr<std::vector<ResponsesEvent>> FlushEvent();

  std::string buffered_;
  std::string event_data_;
  bool finished_ = false;
};

}  // namespace slop

#endif  // SLOP_CORE_RESPONSES_EVENT_DECODER_H_
