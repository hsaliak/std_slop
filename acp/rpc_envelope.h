
#ifndef SLOP_ACP_RPC_ENVELOPE_H_
#define SLOP_ACP_RPC_ENVELOPE_H_

#include <optional>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

namespace slop::acp {

struct RpcRequest {
  std::optional<nlohmann::json> id;
  std::string method;
  nlohmann::json params;

  bool is_notification() const { return !id.has_value(); }
};

// JSON-RPC 2.0 error codes.
inline constexpr int kParseErrorCode = -32700;
inline constexpr int kInvalidRequestCode = -32600;
inline constexpr int kMethodNotFoundCode = -32601;
inline constexpr int kInternalErrorCode = -32603;

absl::StatusOr<RpcRequest> ParseRpcRequest(std::string_view raw);
absl::StatusOr<RpcRequest> ParseRpcRequestJson(const nlohmann::json& request_json);

nlohmann::json MakeErrorResponse(std::optional<nlohmann::json> id, int code, std::string_view message);

inline nlohmann::json MakeMethodNotFoundResponse(std::optional<nlohmann::json> id, std::string_view method) {
  return MakeErrorResponse(std::move(id), kMethodNotFoundCode, std::string("Method not found: ") + std::string(method));
}

inline nlohmann::json MakeInvalidRequestResponse(std::optional<nlohmann::json> id, std::string_view message) {
  return MakeErrorResponse(std::move(id), kInvalidRequestCode, message);
}

inline nlohmann::json MakeParseErrorResponse() {
  return MakeErrorResponse(std::nullopt, kParseErrorCode, "Parse error");
}

inline nlohmann::json MakeInternalErrorResponse(std::optional<nlohmann::json> id, std::string_view message) {
  return MakeErrorResponse(std::move(id), kInternalErrorCode, message);
}

}  // namespace slop::acp

#endif  // SLOP_ACP_RPC_ENVELOPE_H_