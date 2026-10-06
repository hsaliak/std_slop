#include "mcp/client/modern_http.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "mcp/client/http_headers.h"
#include "mcp/client/sse_decoder.h"
#include "mcp/json_rpc.h"

namespace slop::mcp::v2026_07_28 {
namespace {

std::string HeaderValue(const absl::flat_hash_map<std::string, std::string>& headers, absl::string_view name) {
  const auto it = headers.find(absl::AsciiStrToLower(name));
  return it == headers.end() ? std::string() : it->second;
}

}  // namespace

HttpExchange::HttpExchange(HttpExchangeOptions options, HttpClient* http_client)
    : options_(std::move(options)), http_client_(http_client) {}

absl::Status HttpExchange::ValidateOptions() const {
  if (http_client_ == nullptr) {
    return absl::InvalidArgumentError("http_client must not be null");
  }
  if (!absl::StartsWith(options_.endpoint_url, "https://") && !absl::StartsWith(options_.endpoint_url, "http://")) {
    return absl::InvalidArgumentError("MCP endpoint must use HTTP or HTTPS");
  }
  if (options_.max_response_bytes == 0 || options_.max_messages == 0) {
    return absl::InvalidArgumentError("MCP response limits must be positive");
  }
  if (options_.deadline <= absl::ZeroDuration()) {
    return absl::InvalidArgumentError("MCP deadline must be positive");
  }
  return ValidateExtraHeaders(options_.extra_headers);
}

absl::StatusOr<HttpExchangeResult> HttpExchange::Execute(const Request& request, absl::Duration timeout) {
  const absl::Status options_status = ValidateOptions();
  if (!options_status.ok()) return options_status;
  if (timeout <= absl::ZeroDuration()) return absl::DeadlineExceededError("MCP request deadline expired");
  const absl::Duration budget = std::min(options_.deadline, timeout);
  const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  auto encoded_or = EncodeRequest(request);
  if (!encoded_or.ok()) return encoded_or.status();

  for (const auto& [name, value] : options_.extra_headers) {
    encoded_or->headers.push_back(absl::StrCat(name, ": ", value));
  }
  if (options_.bearer_token && !options_.bearer_token->empty()) {
    encoded_or->headers.push_back(absl::StrCat("Authorization: Bearer ", *options_.bearer_token));
  }

  const std::string body = json_dump(encoded_or->body);
  const absl::Duration remaining = budget - absl::FromChrono(std::chrono::steady_clock::now() - start);
  if (remaining <= absl::ZeroDuration()) return absl::DeadlineExceededError("MCP request deadline expired");
  size_t received = 0;
  auto response_or = http_client_->PostOnceStreamWithResponse(
      options_.endpoint_url, body, encoded_or->headers, remaining, options_.max_response_bytes,
      [&](absl::string_view chunk) {
        if (chunk.size() > options_.max_response_bytes - received) {
          return absl::ResourceExhaustedError("MCP response byte limit exceeded");
        }
        received += chunk.size();
        return absl::OkStatus();
      });
  if (!response_or.ok()) {
    HttpExchangeResult failed;
    ProtocolFailure failure;
    failure.message = std::string(response_or.status().message());
    failure.execution =
        absl::IsInvalidArgument(response_or.status()) || absl::IsFailedPrecondition(response_or.status())
            ? ExecutionCertainty::kNotExecuted
            : ExecutionCertainty::kMayHaveExecuted;
    failed.failure = std::move(failure);
    return failed;
  }
  if (response_or->body.size() > options_.max_response_bytes) {
    return absl::ResourceExhaustedError("MCP response byte limit exceeded");
  }
  return DecodeResponse(request, *response_or);
}

void HttpExchange::Cancel() {
  if (http_client_ != nullptr) http_client_->Abort();
}

absl::StatusOr<HttpExchangeResult> HttpExchange::DecodeResponse(const Request& request,
                                                                const HttpResponse& response) const {
  std::vector<nlohmann::json> messages;
  const std::string content_type = absl::AsciiStrToLower(HeaderValue(response.headers, "content-type"));
  if (!response.body.empty() && absl::StrContains(content_type, "application/json")) {
    auto message_or = ParseJsonRpcMessage(response.body);
    if (!message_or.ok()) return message_or.status();
    messages.push_back(std::move(*message_or));
  } else if (!response.body.empty() && absl::StrContains(content_type, "text/event-stream")) {
    SseDecoder decoder;
    auto events_or = decoder.Feed(response.body);
    if (!events_or.ok()) return events_or.status();
    auto final_or = decoder.Finish();
    if (!final_or.ok()) return final_or.status();
    events_or->insert(events_or->end(), std::make_move_iterator(final_or->begin()),
                      std::make_move_iterator(final_or->end()));
    if (events_or->size() > options_.max_messages) {
      return absl::ResourceExhaustedError("MCP SSE message limit exceeded");
    }
    for (const SseEvent& event : *events_or) {
      if (event.data.empty()) continue;
      auto message_or = ParseJsonRpcMessage(event.data);
      if (!message_or.ok()) return message_or.status();
      messages.push_back(std::move(*message_or));
    }
  } else if (!response.body.empty()) {
    return absl::InvalidArgumentError(absl::StrCat("unsupported MCP response content type: ", content_type));
  }
  return NormalizeModernMessages(request, std::move(messages), options_.max_messages, response.status_code);
}

}  // namespace slop::mcp::v2026_07_28
