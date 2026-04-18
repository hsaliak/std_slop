
#ifndef SLOP_ACP_CAPABILITIES_H_
#define SLOP_ACP_CAPABILITIES_H_

#include <cstdint>
#include <string>

#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

namespace slop::acp {

inline constexpr int64_t kStableProtocolVersion = 1;

struct NegotiatedRuntimeOptions {
  bool initialized = false;
  int64_t protocol_version = 0;
  nlohmann::json client_capabilities = nlohmann::json::object();
  nlohmann::json runtime_options = nlohmann::json::object();
};

struct InitializeRequest {
  int64_t protocol_version = 0;
  nlohmann::json client_capabilities;
  nlohmann::json runtime_options;
};

absl::StatusOr<InitializeRequest> ParseInitializeParams(const nlohmann::json& params);

void ApplyInitializeRequest(const InitializeRequest& request, NegotiatedRuntimeOptions* state);

nlohmann::json BuildInitializeResult();

}  // namespace slop::acp

#endif  // SLOP_ACP_CAPABILITIES_H_