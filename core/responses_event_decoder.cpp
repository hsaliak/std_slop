#include "core/responses_event_decoder.h"

#include <algorithm>

#include <utility>

#include "absl/container/flat_hash_map.h"

#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "core/json_utils.h"

namespace slop {
namespace {

ResponsesEventType EventTypeFor(const std::string& type) {
  static const absl::flat_hash_map<std::string, ResponsesEventType> kEventTypes = {
      {"response.output_text.delta", ResponsesEventType::kTextDelta},
      {"response.output_text.done", ResponsesEventType::kTextDone},
      {"response.output_item.added", ResponsesEventType::kOutputItem},
      {"response.output_item.done", ResponsesEventType::kOutputItem},
      {"response.completed", ResponsesEventType::kCompleted},
      {"response.done", ResponsesEventType::kCompleted},
      {"response.failed", ResponsesEventType::kFailed},
      {"response.incomplete", ResponsesEventType::kIncomplete},
  };
  const auto event_type = kEventTypes.find(type);
  return event_type == kEventTypes.end() ? ResponsesEventType::kUnknown : event_type->second;
}

}  // namespace

absl::StatusOr<std::vector<ResponsesEvent>> ResponsesEventDecoder::Feed(absl::string_view chunk) {
  if (finished_) return absl::FailedPreconditionError("Responses event decoder is finished");
  buffered_.append(chunk.data(), chunk.size());
  return ConsumeLines(false);
}

absl::StatusOr<std::vector<ResponsesEvent>> ResponsesEventDecoder::Finish() {
  if (finished_) return absl::FailedPreconditionError("Responses event decoder is finished");
  finished_ = true;
  return ConsumeLines(true);
}

absl::StatusOr<std::vector<ResponsesEvent>> ResponsesEventDecoder::ConsumeLines(bool finish) {
  std::vector<ResponsesEvent> events;
  // Keep an incomplete line in buffered_ until a later transport chunk completes it.
  // A blank line terminates one SSE event and causes its accumulated data lines to flush.
  while (true) {
    const size_t newline = buffered_.find('\n');
    if (newline == std::string::npos) break;
    std::string line = buffered_.substr(0, newline);
    buffered_.erase(0, newline + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) {
      auto event_or = FlushEvent();
      if (!event_or.ok()) return event_or.status();
      events.insert(events.end(), std::make_move_iterator(event_or->begin()), std::make_move_iterator(event_or->end()));
    } else if (absl::StartsWith(line, "data:")) {
      const std::string data = std::string(absl::StripLeadingAsciiWhitespace(line.substr(5)));
      if (data == "[DONE]") {
        auto event_or = FlushEvent();
        if (!event_or.ok()) return event_or.status();
        events.insert(events.end(), std::make_move_iterator(event_or->begin()), std::make_move_iterator(event_or->end()));
      } else {
        if (!event_data_.empty()) event_data_.push_back('\n');
        event_data_.append(data);
      }
    }
  }
  // Feed returns only complete frames; Finish may accept a final data line without a newline.
  if (!finish) return events;
  if (!buffered_.empty()) {
    if (absl::StartsWith(buffered_, "data:")) {
      const std::string data = std::string(absl::StripLeadingAsciiWhitespace(buffered_.substr(5)));
      if (!event_data_.empty()) event_data_.push_back('\n');
      event_data_.append(data);
      buffered_.clear();
    } else {
      return absl::InvalidArgumentError("Incomplete SSE line at end of Responses stream");
    }
  }
  auto event_or = FlushEvent();
  if (!event_or.ok()) return event_or.status();
  events.insert(events.end(), std::make_move_iterator(event_or->begin()), std::make_move_iterator(event_or->end()));
  return events;
}

absl::StatusOr<std::vector<ResponsesEvent>> ResponsesEventDecoder::FlushEvent() {
  if (event_data_.empty()) return std::vector<ResponsesEvent>();
  auto json_or = json_parse(event_data_);
  event_data_.clear();
  if (!json_or) return absl::InvalidArgumentError("Malformed Responses SSE event JSON");
  const std::string type = json_get_or(*json_or, "type", std::string{});
  ResponsesEvent event;
  event.type = EventTypeFor(type);
  event.payload = std::move(*json_or);
  event.text = json_get_or(event.payload, "delta", json_get_or(event.payload, "text", std::string{}));
  return std::vector<ResponsesEvent>{std::move(event)};
}

std::optional<nlohmann::json> ResponsesEventDecoder::NormalizeSsePayload(absl::string_view payload) {
  ResponsesEventDecoder decoder;
  auto events_or = decoder.Feed(payload);
  if (!events_or.ok()) return std::nullopt;
  auto final_events_or = decoder.Finish();
  if (!final_events_or.ok()) return std::nullopt;
  events_or->insert(events_or->end(), std::make_move_iterator(final_events_or->begin()),
                    std::make_move_iterator(final_events_or->end()));
  if (events_or->empty()) return std::nullopt;

  nlohmann::json output = nlohmann::json::array();
  nlohmann::json usage;
  std::string delta_text;
  std::string final_text;
  bool has_message = false;
  absl::flat_hash_map<std::string, size_t> function_call_indexes;
  for (const auto& event : *events_or) {
    if (event.type == ResponsesEventType::kTextDelta) {
      delta_text.append(event.text);
    } else if (event.type == ResponsesEventType::kTextDone) {
      final_text = event.text;
    } else if (event.type == ResponsesEventType::kOutputItem) {
      const auto* item = json_at(event.payload, "item");
      if (item != nullptr && json_get_or(event.payload, "type", std::string{}) == "response.output_item.done") {
        output.push_back(*item);
        if (json_get_or(*item, "type", std::string{}) == "function_call") {
          function_call_indexes[json_get_or(*item, "call_id", std::string{})] = output.size() - 1;
        }
        has_message = has_message || json_get_or(*item, "type", std::string{}) == "message";
      }
    } else if (event.type == ResponsesEventType::kCompleted) {
      const auto* response = json_at(event.payload, "response");
      if (response != nullptr) {
        const auto* response_usage = json_at(*response, "usage");
        if (response_usage != nullptr) usage = *response_usage;
        const auto response_output = json_get<nlohmann::json::array_t>(*response, "output");
        if (response_output) {
          for (const auto& item : *response_output) {
            const bool is_function_call = json_get_or(item, "type", std::string{}) == "function_call";
            const std::string call_id = is_function_call ? json_get_or(item, "call_id", std::string{}) : "";
            const auto existing_function_call = function_call_indexes.find(call_id);
            if (is_function_call && existing_function_call != function_call_indexes.end()) {
              output[existing_function_call->second] = item;
            } else if (std::find(output.begin(), output.end(), item) == output.end()) {
              output.push_back(item);
              if (is_function_call) function_call_indexes[call_id] = output.size() - 1;
            }
            has_message = has_message || json_get_or(item, "type", std::string{}) == "message";
          }
        }
      }
    } else if (event.type == ResponsesEventType::kFailed || event.type == ResponsesEventType::kIncomplete) {
      const auto* error = json_at(event.payload, "error");
      const std::string message = error != nullptr ? json_get_or(*error, "message", std::string{}) : "";
      nlohmann::json normalized = {{"output", output},
                                   {"status", event.type == ResponsesEventType::kFailed ? "failed" : "incomplete"},
                                   {"error", {{"message", message}}}};
      return normalized;
    }
  }
  const std::string text = !final_text.empty() ? final_text : delta_text;
  if (!text.empty() && !has_message) {
    output.push_back({{"type", "message"}, {"role", "assistant"},
                      {"content", nlohmann::json::array({{{"type", "output_text"}, {"text", text}}})}});
  }
  nlohmann::json normalized = {{"output", output}};
  if (!usage.is_null() && !usage.empty()) normalized["usage"] = usage;
  return normalized;
}

}  // namespace slop
