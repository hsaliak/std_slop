#include "mcp/json_rpc.h"

#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "core/json_utils.h"
#include "mcp/protocol.h"

namespace slop::mcp {
namespace {

absl::StatusOr<JsonRpcId> ParseId(const nlohmann::json& id) {
  if (id.is_null()) return std::monostate{};
  if (id.is_number_integer()) return id.get<int64_t>();
  if (id.is_string()) return id.get<std::string>();
  return absl::InvalidArgumentError("JSON-RPC id must be null, an integer, or a string");
}

absl::Status ValidateJsonRpcVersion(const nlohmann::json& message) {
  const std::string version = json_get_or(message, "jsonrpc", std::string{});
  if (version != kJsonRpcVersion) return absl::InvalidArgumentError("JSON-RPC message must contain jsonrpc 2.0");
  return absl::OkStatus();
}

}  // namespace

nlohmann::json BuildJsonRpcRequest(const JsonRpcId& id, absl::string_view method, const nlohmann::json& params) {
  nlohmann::json message = {{"jsonrpc", std::string(kJsonRpcVersion)}, {"method", std::string(method)}};
  if (std::holds_alternative<int64_t>(id)) {
    message["id"] = std::get<int64_t>(id);
  } else if (std::holds_alternative<std::string>(id)) {
    message["id"] = std::get<std::string>(id);
  } else {
    message["id"] = nullptr;
  }
  if (!params.is_null()) message["params"] = params;
  return message;
}

nlohmann::json BuildJsonRpcNotification(absl::string_view method, const nlohmann::json& params) {
  nlohmann::json message = {{"jsonrpc", std::string(kJsonRpcVersion)}, {"method", std::string(method)}};
  if (!params.is_null()) message["params"] = params;
  return message;
}

absl::StatusOr<JsonRpcResponse> ParseJsonRpcResponse(const nlohmann::json& message) {
  if (!message.is_object()) return absl::InvalidArgumentError("JSON-RPC response must be an object");
  const absl::Status version_status = ValidateJsonRpcVersion(message);
  if (!version_status.ok()) return version_status;
  const auto* id_json = json_at(message, "id");
  if (id_json == nullptr) return absl::InvalidArgumentError("JSON-RPC response missing id");
  const bool has_result = json_at(message, "result") != nullptr;
  const bool has_error = json_at(message, "error") != nullptr;
  if (has_result == has_error) {
    return absl::InvalidArgumentError("JSON-RPC response must contain exactly one of result or error");
  }
  JsonRpcResponse response;
  auto id_or = ParseId(*id_json);
  if (!id_or.ok()) return id_or.status();
  response.id = *id_or;
  if (has_result) {
    response.result = *json_at(message, "result");
    return response;
  }
  const auto* error = json_at(message, "error");
  if (!error->is_object()) return absl::InvalidArgumentError("JSON-RPC error must be an object");
  auto code = json_get<int>(*error, "code");
  auto error_message = json_get<std::string>(*error, "message");
  if (!code || !error_message) return absl::InvalidArgumentError("JSON-RPC error missing code or message");
  JsonRpcError parsed_error;
  parsed_error.code = *code;
  parsed_error.message = *error_message;
  if (const auto* data = json_at(*error, "data")) parsed_error.data = *data;
  response.error = std::move(parsed_error);
  return response;
}

absl::StatusOr<nlohmann::json> ParseJsonRpcMessage(absl::string_view raw) {
  auto parsed = json_parse(std::string(raw));
  if (!parsed) return absl::InvalidArgumentError("MCP message is not valid JSON");
  if (!parsed->is_object()) return absl::InvalidArgumentError("MCP message must be a JSON object");
  const absl::Status version_status = ValidateJsonRpcVersion(*parsed);
  if (!version_status.ok()) return version_status;
  const bool has_method = json_get<std::string>(*parsed, "method").has_value();
  const bool has_id = json_at(*parsed, "id") != nullptr;
  const bool has_result = json_at(*parsed, "result") != nullptr;
  const bool has_error = json_at(*parsed, "error") != nullptr;
  if (has_method) {
    if (has_result || has_error) return absl::InvalidArgumentError("JSON-RPC request cannot contain result or error");
    if (has_id) {
      auto id_or = ParseId(*json_at(*parsed, "id"));
      if (!id_or.ok()) return id_or.status();
    }
    return *parsed;
  }
  auto response_or = ParseJsonRpcResponse(*parsed);
  if (!response_or.ok()) return response_or.status();
  return *parsed;
}

std::string JsonRpcIdToString(const JsonRpcId& id) {
  if (std::holds_alternative<int64_t>(id)) return absl::StrCat(std::get<int64_t>(id));
  if (std::holds_alternative<std::string>(id)) return std::get<std::string>(id);
  return "null";
}

}  // namespace slop::mcp
