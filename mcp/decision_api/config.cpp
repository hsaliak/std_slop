#include "mcp/decision_api/config.h"

#include <string>

#include "absl/status/status.h"

#include "core/json_utils.h"
#include "mcp/validation.h"

namespace slop::mcp::decision_api {
namespace {

constexpr std::size_t kMaxConfigBytes = 64 * 1024;
constexpr std::int64_t kMaxTimeoutMs = 60'000;

}  // namespace

absl::StatusOr<Config> ParseConfig(const nlohmann::json& value) {
  const absl::Status fields_status = ValidateJsonObjectFields(
      value, {"apiKey", "endpoint", "modelsEndpoint", "model", "timeoutMs"}, "decision API config");
  if (!fields_status.ok()) return fields_status;

  const auto api_key = json_get<std::string>(value, "apiKey");
  const auto model = json_get<std::string>(value, "model");
  if (!api_key.has_value() || api_key->empty() || ContainsControlCharacters(*api_key)) {
    return absl::InvalidArgumentError("config requires a non-empty apiKey without control characters");
  }
  if (!model.has_value() || model->empty() || ContainsControlCharacters(*model) || model->size() > 256) {
    return absl::InvalidArgumentError("config requires a non-empty model ID or alias of at most 256 bytes");
  }

  Config config;
  config.api_key = *api_key;
  config.model = *model;
  if (const auto* endpoint_value = json_at(value, "endpoint"); endpoint_value != nullptr) {
    const auto endpoint = json_get<std::string>(value, "endpoint");
    if (!endpoint.has_value() || !IsValidHttpUrl(*endpoint, 2048, true)) {
      return absl::InvalidArgumentError("endpoint must be a valid HTTPS URL without userinfo or fragment");
    }
    config.endpoint = *endpoint;
  }

  if (const auto* models_value = json_at(value, "modelsEndpoint"); models_value != nullptr) {
    const auto models_endpoint = json_get<std::string>(value, "modelsEndpoint");
    if (!models_endpoint.has_value() || !IsValidHttpUrl(*models_endpoint, 2048, true)) {
      return absl::InvalidArgumentError("modelsEndpoint must be a valid HTTPS URL without userinfo or fragment");
    }
    config.models_endpoint = *models_endpoint;
  } else if (config.endpoint != kDefaultEndpoint) {
    config.models_endpoint = std::nullopt;
  }

  if (const auto* timeout_value = json_at(value, "timeoutMs"); timeout_value != nullptr) {
    const auto timeout = json_get<std::int64_t>(value, "timeoutMs");
    if (!timeout.has_value() || *timeout < 1 || *timeout > kMaxTimeoutMs) {
      return absl::InvalidArgumentError("timeoutMs must be an integer from 1 to 60000");
    }
    config.timeout_ms = *timeout;
  }
  return config;
}

absl::StatusOr<Config> ParseConfigText(const std::string& text) {
  if (text.size() > kMaxConfigBytes) return absl::ResourceExhaustedError("decision API config exceeds 64 KiB");
  auto value = json_parse(text);
  if (!value.has_value()) return absl::InvalidArgumentError("decision API config is not valid JSON");
  return ParseConfig(*value);
}

}  // namespace slop::mcp::decision_api
