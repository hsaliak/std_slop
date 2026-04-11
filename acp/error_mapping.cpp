
#include "acp/error_mapping.h"

#include "acp/rpc_envelope.h"

namespace slop::acp {

AcpError MakeParseError(std::string_view message) {
  return AcpError{.code = kParseErrorCode, .message = std::string(message)};
}

AcpError MakeInvalidRequestError(std::string_view message) {
  return AcpError{.code = kInvalidRequestCode, .message = std::string(message)};
}

AcpError MakeMethodNotFoundError(std::string_view method_name) {
  return AcpError{.code = kMethodNotFoundCode, .message = std::string("Method not found: ") + std::string(method_name)};
}

AcpError MakeInternalError(std::string_view message) {
  return AcpError{.code = kInternalErrorCode, .message = std::string(message)};
}

AcpError MapStatusToAcpError(const absl::Status& status) {
  switch (status.code()) {
    case absl::StatusCode::kInvalidArgument:
    case absl::StatusCode::kAlreadyExists:
    case absl::StatusCode::kNotFound:
    case absl::StatusCode::kFailedPrecondition:
      return MakeInvalidRequestError(status.message());
    case absl::StatusCode::kInternal:
      return MakeInternalError(status.message());
    default:
      return MakeInvalidRequestError(status.message());
  }
}

nlohmann::json MakeAcpErrorResponse(std::optional<nlohmann::json> id, const AcpError& error) {
  nlohmann::json response = {
      {"jsonrpc", "2.0"},
      {"error", nlohmann::json{{"code", error.code}, {"message", error.message}}},
  };
  if (id.has_value()) {
    response["id"] = *id;
  } else {
    response["id"] = nullptr;
  }
  return response;
}

}  // namespace slop::acp
