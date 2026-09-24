#ifndef SLOP_MCP_SSE_DECODER_H_
#define SLOP_MCP_SSE_DECODER_H_

#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace slop::mcp {

struct SseEvent {
  std::string event;
  std::string data;
  std::string id;
};

class SseDecoder {
 public:
  absl::StatusOr<std::vector<SseEvent>> Feed(absl::string_view chunk);
  absl::StatusOr<std::vector<SseEvent>> Finish();

 private:
  absl::StatusOr<std::vector<SseEvent>> ConsumeLines(bool finish);
  void ConsumeLine(absl::string_view line);
  std::vector<SseEvent> FlushEvent();

  std::string buffered_;
  SseEvent pending_;
  bool has_pending_field_ = false;
  bool finished_ = false;
};

}  // namespace slop::mcp

#endif  // SLOP_MCP_SSE_DECODER_H_
