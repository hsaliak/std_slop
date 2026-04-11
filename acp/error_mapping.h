#ifndef SLOP_ACP_ERROR_MAPPING_H_
#define SLOP_ACP_ERROR_MAPPING_H_

#include <optional>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "nlohmann/json.hpp"

namespace slop::acp {

struct AcpError {
  int code;
  std::string message;
};

AcpError MakeParseError(std::string_view message = "Parse error");
AcpError MakeInvalidRequestError(std::string_view message = "Invalid request");
AcpError MakeMethodNotFoundError(std::string_view method_name);
AcpError MakeInternalError(std::string_view message = "Internal error");

// Maps ACP execution status values into stable JSON-RPC error space.
AcpError MapStatusToAcpError(const absl::Status& status);

nlohmann::json MakeAcpErrorResponse(std::optional<nlohmann::json> id, const AcpError& error);

inline nlohmann::json MakeInvalidRequestResponseFromStatus(std::optional<nlohmann::json> id,
                                                           const absl::Status& status) {
  return MakeAcpErrorResponse(std::move(id), MakeInvalidRequestError(status.message()));
}

inline nlohmann::json MakeMappedErrorResponse(std::optional<nlohmann::json> id, const absl::Status& status) {
  return MakeAcpErrorResponse(std::move(id), MapStatusToAcpError(status));
}

}  // namespace slop::acp

#endif  // SLOP_ACP_ERROR_MAPPING_H_
