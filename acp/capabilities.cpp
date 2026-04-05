
#include "acp/capabilities.h"

#include "absl/status/status.h"
#include "core/json_utils.h"

namespace slop::acp {

absl::StatusOr<InitializeRequest> ParseInitializeParams(const nlohmann::json& params) {
  if (!params.is_object()) {
    return absl::InvalidArgumentError("initialize_params_must_be_object");
  }

  auto version = json_get<std::string>(params, "protocolVersion");
  if (!version.has_value() || version->empty()) {
    return absl::InvalidArgumentError("initialize_protocol_version_required");
  }
  if (*version != kStableProtocolVersion) {
    return absl::InvalidArgumentError("unsupported_protocol_version");
  }

  if (!params.contains("capabilities") || !params.at("capabilities").is_object()) {
    return absl::InvalidArgumentError("initialize_capabilities_must_be_object");
  }

  if (params.contains("runtimeOptions") && !params.at("runtimeOptions").is_object()) {
    return absl::InvalidArgumentError("initialize_runtime_options_must_be_object");
  }

  InitializeRequest req;
  req.protocol_version = *version;
  req.client_capabilities = params.at("capabilities");
  req.runtime_options = params.value("runtimeOptions", nlohmann::json::object());
  return req;
}

void ApplyInitializeRequest(const InitializeRequest& request, NegotiatedRuntimeOptions* state) {
  state->initialized = true;
  state->protocol_version = request.protocol_version;
  state->client_capabilities = request.client_capabilities;
  state->runtime_options = request.runtime_options;
}

nlohmann::json BuildInitializeResult() {
  return nlohmann::json({
      {"protocolVersion", kStableProtocolVersion},
      {"capabilities",
       nlohmann::json({
           {"session", nlohmann::json({{"new", true}, {"prompt", true}, {"cancel", true}, {"update", true}})},
       })},
  });
}

}  // namespace slop::acp