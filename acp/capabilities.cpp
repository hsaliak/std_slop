
#include "acp/capabilities.h"

#include <string>

#include "absl/status/status.h"
#include "core/json_utils.h"

namespace slop::acp {

absl::StatusOr<InitializeRequest> ParseInitializeParams(const nlohmann::json& params) {
  if (!params.is_object()) {
    return absl::InvalidArgumentError("initialize_params_must_be_object");
  }

  std::string version;
  const nlohmann::json version_value = json_get_or<nlohmann::json>(params, "protocolVersion", nlohmann::json());
  if (version_value.is_string()) {
    version = version_value.get<std::string>();
  } else if (version_value.is_number_integer()) {
    version = std::to_string(version_value.get<int64_t>());
  } else if (version_value.is_number_unsigned()) {
    version = std::to_string(version_value.get<uint64_t>());
  }
  if (version.empty()) {
    return absl::InvalidArgumentError("initialize_protocol_version_required");
  }
  if (version != kStableProtocolVersion) {
    return absl::InvalidArgumentError("unsupported_protocol_version");
  }

  nlohmann::json capabilities = nlohmann::json::object();
  const nlohmann::json explicit_capabilities = json_get_or<nlohmann::json>(params, "capabilities", nlohmann::json());
  if (!explicit_capabilities.is_null()) {
    if (!explicit_capabilities.is_object()) {
      return absl::InvalidArgumentError("initialize_capabilities_must_be_object");
    }
    capabilities = explicit_capabilities;
  } else {
    const nlohmann::json client_capabilities =
        json_get_or<nlohmann::json>(params, "clientCapabilities", nlohmann::json());
    if (!client_capabilities.is_null()) {
      if (!client_capabilities.is_object()) {
        return absl::InvalidArgumentError("initialize_capabilities_must_be_object");
      }
      capabilities = client_capabilities;
    }
  }

  const nlohmann::json runtime_options = json_get_or<nlohmann::json>(params, "runtimeOptions", nlohmann::json::object());
  if (!runtime_options.is_object()) {
    return absl::InvalidArgumentError("initialize_runtime_options_must_be_object");
  }

  InitializeRequest req;
  req.protocol_version = version;
  req.client_capabilities = capabilities;
  req.runtime_options = runtime_options;
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