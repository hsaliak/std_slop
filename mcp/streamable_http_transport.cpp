#include "mcp/streamable_http_transport.h"

#include <utility>

#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "core/json_utils.h"
#include "mcp/json_rpc.h"
#include "mcp/protocol.h"
#include "mcp/sse_decoder.h"

namespace slop::mcp {
namespace {

std::string HeaderValue(const absl::flat_hash_map<std::string, std::string>& headers, absl::string_view key) {
  auto it = headers.find(std::string(absl::AsciiStrToLower(key)));
  return it == headers.end() ? std::string() : it->second;
}

absl::Status HttpStatusToStatus(long status_code) {
  if (status_code == 401) return absl::UnauthenticatedError("MCP server requires authentication");
  if (status_code == 403) return absl::PermissionDeniedError("MCP server denied the request");
  return absl::UnavailableError(absl::StrCat("MCP HTTP request failed with status ", status_code));
}

}  // namespace

StreamableHttpTransport::StreamableHttpTransport(StreamableHttpConfig config, HttpClient* http_client)
    : config_(std::move(config)), http_client_(http_client) {}

absl::Status StreamableHttpTransport::Start() {
  if (http_client_ == nullptr) return absl::InvalidArgumentError("http_client must not be null");
  if (config_.endpoint_url.empty()) return absl::InvalidArgumentError("MCP endpoint_url must not be empty");
  if (closed_) return absl::FailedPreconditionError("MCP transport is closed");
  started_ = true;
  return absl::OkStatus();
}

absl::Status StreamableHttpTransport::Send(const nlohmann::json& message) {
  if (!started_) return absl::FailedPreconditionError("MCP transport is not started");
  if (closed_) return absl::FailedPreconditionError("MCP transport is closed");
  if (!message.is_object()) return absl::InvalidArgumentError("MCP outbound message must be an object");

  auto outbound_or = ParseJsonRpcMessage(json_dump(message));
  if (!outbound_or.ok()) return outbound_or.status();

  auto response_or = http_client_->PostStreamWithResponse(config_.endpoint_url, json_dump(message), BuildHeaders(),
                                                          [](absl::string_view) { return absl::OkStatus(); });
  if (!response_or.ok()) return response_or.status();
  return EnqueueResponseMessages(*response_or);
}

absl::StatusOr<nlohmann::json> StreamableHttpTransport::Receive(absl::Duration /*timeout*/) {
  if (!started_) return absl::FailedPreconditionError("MCP transport is not started");
  if (closed_) return absl::FailedPreconditionError("MCP transport is closed");
  if (pending_messages_.empty()) return absl::UnavailableError("No MCP response message is available");
  nlohmann::json message = std::move(pending_messages_.front());
  pending_messages_.erase(pending_messages_.begin());
  return message;
}

absl::Status StreamableHttpTransport::Close() {
  closed_ = true;
  pending_messages_.clear();
  return absl::OkStatus();
}

std::vector<std::string> StreamableHttpTransport::BuildHeaders() const {
  std::vector<std::string> headers = {"Content-Type: application/json", "Accept: application/json, text/event-stream"};
  if (!protocol_version_.empty()) headers.push_back(absl::StrCat(kProtocolVersionHeader, ": ", protocol_version_));
  if (!session_id_.empty()) headers.push_back(absl::StrCat(kSessionIdHeader, ": ", session_id_));
  if (config_.bearer_token.has_value() && !config_.bearer_token->empty()) {
    headers.push_back(absl::StrCat("Authorization: Bearer ", *config_.bearer_token));
  }
  for (const auto& [key, value] : config_.extra_headers) {
    headers.push_back(absl::StrCat(key, ": ", value));
  }
  return headers;
}

absl::Status StreamableHttpTransport::EnqueueResponseMessages(const HttpResponse& response) {
  if (response.status_code < 200 || response.status_code >= 300) return HttpStatusToStatus(response.status_code);

  const std::string session_id = HeaderValue(response.headers, kSessionIdHeader);
  if (!session_id.empty()) session_id_ = session_id;

  if (response.body.empty()) return absl::OkStatus();

  const std::string content_type = absl::AsciiStrToLower(HeaderValue(response.headers, "content-type"));
  if (absl::StrContains(content_type, "application/json")) {
    auto message_or = ParseJsonRpcMessage(response.body);
    if (!message_or.ok()) return message_or.status();
    pending_messages_.push_back(std::move(*message_or));
    return absl::OkStatus();
  }
  if (absl::StrContains(content_type, "text/event-stream")) {
    SseDecoder decoder;
    auto events_or = decoder.Feed(response.body);
    if (!events_or.ok()) return events_or.status();
    auto final_events_or = decoder.Finish();
    if (!final_events_or.ok()) return final_events_or.status();
    events_or->insert(events_or->end(), std::make_move_iterator(final_events_or->begin()),
                      std::make_move_iterator(final_events_or->end()));
    for (const SseEvent& event : *events_or) {
      if (event.data.empty()) continue;
      auto message_or = ParseJsonRpcMessage(event.data);
      if (!message_or.ok()) return message_or.status();
      pending_messages_.push_back(std::move(*message_or));
    }
    return absl::OkStatus();
  }
  return absl::InvalidArgumentError(absl::StrCat("Unsupported MCP response content type: ", content_type));
}

}  // namespace slop::mcp
