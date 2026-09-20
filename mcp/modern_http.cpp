#include "mcp/modern_http.h"

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
#include "absl/strings/strip.h"

#include "core/json_utils.h"
#include "mcp/json_rpc.h"
#include "mcp/sse_decoder.h"

namespace slop::mcp::v2026_07_28 {
namespace {

std::string HeaderValue(const absl::flat_hash_map<std::string, std::string>& headers, absl::string_view name) {
  const auto it = headers.find(absl::AsciiStrToLower(name));
  return it == headers.end() ? std::string() : it->second;
}

bool IsReservedHeader(absl::string_view name) {
  const std::string lower = absl::AsciiStrToLower(name);
  return lower == "content-type" || lower == "accept" || lower == "authorization" || lower == "mcp-protocol-version" ||
         lower == "mcp-method" || lower == "mcp-name" || lower == "mcp-session-id" || lower == "last-event-id" ||
         absl::StartsWith(lower, "mcp-param-");
}

absl::StatusOr<ServerNotification> ParseNotification(const nlohmann::json& message) {
  if (!message.is_object() || json_at(message, "id") != nullptr) {
    return absl::InvalidArgumentError("invalid MCP notification envelope");
  }
  const std::string method = json_get_or(message, "method", std::string{});
  if (method.empty()) {
    return absl::InvalidArgumentError("MCP notification missing method");
  }
  ServerNotification notification;
  if (method == "notifications/progress") {
    notification.kind = ServerNotificationKind::kProgress;
  } else if (method == "notifications/message") {
    notification.kind = ServerNotificationKind::kLogging;
  } else if (method == "notifications/tools/list_changed") {
    notification.kind = ServerNotificationKind::kToolsListChanged;
  } else if (method == "notifications/resources/list_changed") {
    notification.kind = ServerNotificationKind::kResourcesListChanged;
  } else if (method == "notifications/prompts/list_changed") {
    notification.kind = ServerNotificationKind::kPromptsListChanged;
  } else if (method == "notifications/subscriptions/acknowledged") {
    notification.kind = ServerNotificationKind::kSubscriptionAcknowledged;
  } else {
    notification.kind = ServerNotificationKind::kUnknown;
  }
  if (const auto* params = json_at(message, "params")) {
    if (!params->is_object()) {
      return absl::InvalidArgumentError("MCP notification params must be an object");
    }
    notification.params = *params;
  }
  return notification;
}

ExecutionCertainty ErrorCertainty(long http_status, const JsonRpcResponse* response) {
  if (http_status == 401 || http_status == 403 || http_status == 404) {
    return ExecutionCertainty::kNotExecuted;
  }
  if (http_status == 400 && response != nullptr && response->error) {
    const int code = response->error->code;
    if (code == -32602 || code == -32601 || code == -32020 || code == -32021 || code == -32022) {
      return ExecutionCertainty::kNotExecuted;
    }
  }
  return ExecutionCertainty::kMayHaveExecuted;
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
  for (const auto& [name, value] : options_.extra_headers) {
    const absl::string_view trimmed = absl::StripAsciiWhitespace(name);
    if (trimmed.empty() || IsReservedHeader(trimmed)) {
      return absl::InvalidArgumentError(absl::StrCat("reserved or empty MCP header: ", name));
    }
    if (name.find(':') != std::string::npos || value.find('\r') != std::string::npos ||
        value.find('\n') != std::string::npos) {
      return absl::InvalidArgumentError("invalid MCP extra header");
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<HttpExchangeResult> HttpExchange::Execute(const Request& request) {
  const absl::Status options_status = ValidateOptions();
  if (!options_status.ok()) return options_status;
  auto encoded_or = EncodeRequest(request);
  if (!encoded_or.ok()) return encoded_or.status();

  for (const auto& [name, value] : options_.extra_headers) {
    encoded_or->headers.push_back(absl::StrCat(name, ": ", value));
  }
  if (options_.bearer_token && !options_.bearer_token->empty()) {
    encoded_or->headers.push_back(absl::StrCat("Authorization: Bearer ", *options_.bearer_token));
  }

  size_t received = 0;
  auto response_or = http_client_->PostOnceStreamWithResponse(
      options_.endpoint_url, json_dump(encoded_or->body), encoded_or->headers, options_.deadline,
      options_.max_response_bytes, [&](absl::string_view chunk) {
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
  HttpExchangeResult decoded;
  decoded.http_status = response.status_code;
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
  if (messages.size() > options_.max_messages) {
    return absl::ResourceExhaustedError("MCP message limit exceeded");
  }

  for (const nlohmann::json& message : messages) {
    if (json_at(message, "method") != nullptr) {
      auto notification_or = ParseNotification(message);
      if (!notification_or.ok()) return notification_or.status();
      decoded.notifications.push_back(std::move(*notification_or));
      continue;
    }
    auto response_or = ParseJsonRpcResponse(message);
    if (!response_or.ok()) return response_or.status();
    const bool non_success_null_id =
        (response.status_code < 200 || response.status_code >= 300) &&
        std::holds_alternative<std::monostate>(response_or->id);
    if (response_or->id != request.id && !non_success_null_id) {
      return absl::InvalidArgumentError("MCP response id does not match request");
    }
    if (decoded.response) {
      return absl::InvalidArgumentError("MCP exchange returned multiple final responses");
    }
    decoded.response = std::move(*response_or);
  }

  if (response.status_code < 200 || response.status_code >= 300) {
    ProtocolFailure failure;
    failure.http_status = response.status_code;
    failure.message = absl::StrCat("MCP HTTP request failed with status ", response.status_code);
    if (decoded.response && decoded.response->error) {
      failure.message = decoded.response->error->message;
      failure.json_rpc_code = decoded.response->error->code;
      failure.json_rpc_data = decoded.response->error->data;
    }
    failure.execution = ErrorCertainty(response.status_code, decoded.response ? &*decoded.response : nullptr);
    decoded.failure = std::move(failure);
    return decoded;
  }
  if (!decoded.response) {
    return absl::InvalidArgumentError("successful MCP exchange missing final response");
  }
  if (decoded.response->error) {
    ProtocolFailure failure;
    failure.message = decoded.response->error->message;
    failure.json_rpc_code = decoded.response->error->code;
    failure.json_rpc_data = decoded.response->error->data;
    failure.http_status = response.status_code;
    failure.execution = ExecutionCertainty::kNotExecuted;
    decoded.failure = std::move(failure);
  }
  return decoded;
}

}  // namespace slop::mcp::v2026_07_28
