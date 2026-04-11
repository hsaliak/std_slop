
#include "acp/rpc_envelope.h"

#include "absl/status/status.h"
#include "core/json_utils.h"

namespace slop::acp {

namespace {

bool IsValidRpcIdType(const nlohmann::json& id) {
  return id.is_null() || id.is_string() || id.is_number();
}

absl::StatusOr<std::optional<nlohmann::json>> ParseRpcId(const nlohmann::json& request_json) {
  const nlohmann::json* id = json_at(request_json, "id");
  if (id == nullptr) {
    return std::nullopt;
  }
  if (!IsValidRpcIdType(*id)) {
    return absl::InvalidArgumentError("id_must_be_string_number_or_null");
  }
  return *id;
}

absl::StatusOr<nlohmann::json> ParseRpcParams(const nlohmann::json& request_json) {
  const nlohmann::json params = json_get_or<nlohmann::json>(request_json, "params", nlohmann::json::object());
  if (!params.is_object() && !params.is_array()) {
    return absl::InvalidArgumentError("params_must_be_object_or_array");
  }
  return params;
}

}  // namespace

absl::StatusOr<RpcRequest> ParseRpcRequest(std::string_view raw) {
  auto parsed = json_parse(raw);
  if (!parsed.has_value()) {
    return absl::DataLossError("invalid_json");
  }
  return ParseRpcRequestJson(*parsed);
}

bool IsRpcParseError(const absl::Status& status) {
  return status.code() == absl::StatusCode::kDataLoss;
}

absl::StatusOr<RpcRequest> ParseRpcRequestJson(const nlohmann::json& request_json) {
  if (!request_json.is_object()) {
    return absl::InvalidArgumentError("request_must_be_object");
  }

  auto version = json_get<std::string>(request_json, "jsonrpc");
  if (!version.has_value() || *version != "2.0") {
    return absl::InvalidArgumentError("jsonrpc_must_be_2_0");
  }

  auto method = json_get<std::string>(request_json, "method");
  if (!method.has_value() || method->empty()) {
    return absl::InvalidArgumentError("method_must_be_nonempty_string");
  }

  RpcRequest request;
  auto id_or = ParseRpcId(request_json);
  if (!id_or.ok()) {
    return id_or.status();
  }
  request.id = *id_or;

  request.method = *method;
  auto params_or = ParseRpcParams(request_json);
  if (!params_or.ok()) {
    return params_or.status();
  }
  request.params = *params_or;

  return request;
}

nlohmann::json MakeErrorResponse(std::optional<nlohmann::json> id, int code, std::string_view message) {
  nlohmann::json error = {
      {"code", code},
      {"message", std::string(message)},
  };
  nlohmann::json response = {
      {"jsonrpc", "2.0"},
      {"error", error},
  };
  if (id.has_value()) {
    response["id"] = *id;
  } else {
    response["id"] = nullptr;
  }
  return response;
}

}  // namespace slop::acp