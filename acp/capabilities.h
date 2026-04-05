
#ifndef SLOP_ACP_CAPABILITIES_H_
#define SLOP_ACP_CAPABILITIES_H_

#include <string>

#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

namespace slop::acp {

inline constexpr char kStableProtocolVersion[] = "1";

struct NegotiatedRuntimeOptions {
  bool initialized = false;
  std::string protocol_version;
  nlohmann::json client_capabilities = nlohmann::json::object();
  nlohmann::json runtime_options = nlohmann::json::object();
};

struct InitializeRequest {
  std::string protocol_version;
  nlohmann::json client_capabilities;
  nlohmann::json runtime_options;
};

absl::StatusOr<InitializeRequest> ParseInitializeParams(const nlohmann::json& params);

void ApplyInitializeRequest(const InitializeRequest& request, NegotiatedRuntimeOptions* state);

nlohmann::json BuildInitializeResult();

}  // namespace slop::acp

#endif  // SLOP_ACP_CAPABILITIES_H_