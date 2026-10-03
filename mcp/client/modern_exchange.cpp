#include "mcp/client/modern_exchange.h"

#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "mcp/json_rpc.h"

namespace slop::mcp::v2026_07_28 {
namespace {

absl::StatusOr<ServerNotification> ParseNotification(const nlohmann::json& message) {
  if (!message.is_object() || json_at(message, "id") != nullptr) {
    return absl::InvalidArgumentError("invalid MCP notification envelope");
  }
  const std::string method = json_get_or(message, "method", std::string{});
  if (method.empty()) return absl::InvalidArgumentError("MCP notification missing method");

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
    if (!params->is_object()) return absl::InvalidArgumentError("MCP notification params must be an object");
    notification.params = *params;
  }
  return notification;
}

ExecutionCertainty ErrorCertainty(long http_status, const JsonRpcResponse* response) {
  if (http_status == 401 || http_status == 403 || http_status == 404) return ExecutionCertainty::kNotExecuted;
  if (http_status == 400 && response != nullptr && response->error) {
    const int code = response->error->code;
    if (code == -32602 || code == -32601 || code == -32020 || code == -32021 || code == -32022) {
      return ExecutionCertainty::kNotExecuted;
    }
  }
  return ExecutionCertainty::kMayHaveExecuted;
}

}  // namespace

absl::StatusOr<ModernExchangeResult> NormalizeModernMessages(const Request& request,
                                                             std::vector<nlohmann::json> messages, size_t max_messages,
                                                             std::optional<long> http_status) {
  if (max_messages == 0) return absl::InvalidArgumentError("MCP message limit must be positive");
  if (messages.size() > max_messages) return absl::ResourceExhaustedError("MCP message limit exceeded");

  ModernExchangeResult decoded;
  for (const nlohmann::json& message : messages) {
    if (json_at(message, "method") != nullptr) {
      auto notification_or = ParseNotification(message);
      if (!notification_or.ok()) return notification_or.status();
      decoded.notifications.push_back(std::move(*notification_or));
      continue;
    }
    auto response_or = ParseJsonRpcResponse(message);
    if (!response_or.ok()) return response_or.status();
    const bool non_success_null_id = http_status.has_value() && (*http_status < 200 || *http_status >= 300) &&
                                     std::holds_alternative<std::monostate>(response_or->id);
    if (response_or->id != request.id && !non_success_null_id) {
      return absl::InvalidArgumentError("MCP response id does not match request");
    }
    if (decoded.response) return absl::InvalidArgumentError("MCP exchange returned multiple final responses");
    decoded.response = std::move(*response_or);
  }

  if (http_status.has_value() && (*http_status < 200 || *http_status >= 300)) {
    ProtocolFailure failure;
    failure.http_status = *http_status;
    failure.message = absl::StrCat("MCP HTTP request failed with status ", *http_status);
    if (decoded.response && decoded.response->error) {
      failure.message = decoded.response->error->message;
      failure.json_rpc_code = decoded.response->error->code;
      failure.json_rpc_data = decoded.response->error->data;
    }
    failure.execution = ErrorCertainty(*http_status, decoded.response ? &*decoded.response : nullptr);
    decoded.failure = std::move(failure);
    return decoded;
  }
  if (!decoded.response) return absl::InvalidArgumentError("successful MCP exchange missing final response");
  if (decoded.response->error) {
    ProtocolFailure failure;
    failure.message = decoded.response->error->message;
    failure.json_rpc_code = decoded.response->error->code;
    failure.json_rpc_data = decoded.response->error->data;
    failure.http_status = http_status;
    failure.execution = ExecutionCertainty::kNotExecuted;
    decoded.failure = std::move(failure);
  }
  return decoded;
}

}  // namespace slop::mcp::v2026_07_28
