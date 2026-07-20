#include "mcp/sse_decoder.h"

#include <iterator>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/match.h"

namespace slop::mcp {

absl::StatusOr<std::vector<SseEvent>> SseDecoder::Feed(absl::string_view chunk) {
  if (finished_) return absl::FailedPreconditionError("SSE decoder is finished");
  buffered_.append(chunk.data(), chunk.size());
  return ConsumeLines(false);
}

absl::StatusOr<std::vector<SseEvent>> SseDecoder::Finish() {
  if (finished_) return absl::FailedPreconditionError("SSE decoder is finished");
  finished_ = true;
  return ConsumeLines(true);
}

absl::StatusOr<std::vector<SseEvent>> SseDecoder::ConsumeLines(bool finish) {
  std::vector<SseEvent> events;
  while (true) {
    const size_t newline = buffered_.find('\n');
    if (newline == std::string::npos) break;
    std::string line = buffered_.substr(0, newline);
    buffered_.erase(0, newline + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) {
      std::vector<SseEvent> flushed = FlushEvent();
      events.insert(events.end(), std::make_move_iterator(flushed.begin()), std::make_move_iterator(flushed.end()));
      continue;
    }
    ConsumeLine(line);
  }
  if (!finish) return events;
  if (!buffered_.empty()) {
    if (buffered_.back() == '\r') buffered_.pop_back();
    if (!buffered_.empty()) ConsumeLine(buffered_);
    buffered_.clear();
  }
  std::vector<SseEvent> flushed = FlushEvent();
  events.insert(events.end(), std::make_move_iterator(flushed.begin()), std::make_move_iterator(flushed.end()));
  return events;
}

void SseDecoder::ConsumeLine(absl::string_view line) {
  if (line.empty() || line[0] == ':') return;
  const size_t colon = line.find(':');
  const absl::string_view field = colon == absl::string_view::npos ? line : line.substr(0, colon);
  absl::string_view value = colon == absl::string_view::npos ? absl::string_view() : line.substr(colon + 1);
  if (absl::StartsWith(value, " ")) value.remove_prefix(1);

  if (field == "data") {
    if (!pending_.data.empty()) pending_.data.push_back('\n');
    pending_.data.append(value.data(), value.size());
    has_pending_field_ = true;
  } else if (field == "event") {
    pending_.event.assign(value.data(), value.size());
    has_pending_field_ = true;
  } else if (field == "id") {
    pending_.id.assign(value.data(), value.size());
    has_pending_field_ = true;
  }
}

std::vector<SseEvent> SseDecoder::FlushEvent() {
  if (!has_pending_field_) return {};
  SseEvent event = std::move(pending_);
  pending_ = SseEvent();
  has_pending_field_ = false;
  return {std::move(event)};
}

}  // namespace slop::mcp
