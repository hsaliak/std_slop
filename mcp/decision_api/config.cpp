#include "mcp/decision_api/config.h"

#include <string>
#include <unordered_set>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"

namespace slop::mcp::decision_api {
namespace {

constexpr std::size_t kMaxConfigBytes = 64 * 1024;
constexpr std::int64_t kMaxTimeoutMs = 60'000;

bool ContainsControl(const std::string& value) {
  for (unsigned char character : value) {
    if (character < 0x20 || character == 0x7f) return true;
  }
  return false;
}

bool IsValidEndpoint(const std::string& endpoint) {
  if (endpoint.rfind("https://", 0) != 0 || endpoint.size() > 2048 || endpoint.find('#') != std::string::npos ||
      endpoint.find('@') != std::string::npos || endpoint.find(' ') != std::string::npos || ContainsControl(endpoint)) {
    return false;
  }
  const std::size_t authority_begin = 8;
  const std::size_t authority_end = endpoint.find_first_of("/?", authority_begin);
  const std::string authority = endpoint.substr(
      authority_begin, authority_end == std::string::npos ? std::string::npos : authority_end - authority_begin);
  return !authority.empty() && authority != "." && authority.find('\\') == std::string::npos;
}

}  // namespace

absl::StatusOr<Config> ParseConfig(const nlohmann::json& value) {
  if (!value.is_object()) return absl::InvalidArgumentError("decision API config must be an object");
  static const std::unordered_set<std::string> allowed_fields = {"apiKey", "endpoint", "modelsEndpoint", "model",
                                                                 "timeoutMs"};
  for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
    if (allowed_fields.find(iterator.key()) == allowed_fields.end()) {
      return absl::InvalidArgumentError(absl::StrCat("unknown config field: ", iterator.key()));
    }
  }

  const auto api_key = json_get<std::string>(value, "apiKey");
  const auto model = json_get<std::string>(value, "model");
  if (!api_key.has_value() || api_key->empty() || ContainsControl(*api_key)) {
    return absl::InvalidArgumentError("config requires a non-empty apiKey without control characters");
  }
  if (!model.has_value() || model->empty() || ContainsControl(*model) || model->size() > 256) {
    return absl::InvalidArgumentError("config requires a non-empty model ID or alias of at most 256 bytes");
  }

  Config config;
  config.api_key = *api_key;
  config.model = *model;
  if (const auto* endpoint_value = json_at(value, "endpoint"); endpoint_value != nullptr) {
    const auto endpoint = json_get<std::string>(value, "endpoint");
    if (!endpoint.has_value() || !IsValidEndpoint(*endpoint)) {
      return absl::InvalidArgumentError("endpoint must be a valid HTTPS URL without userinfo or fragment");
    }
    config.endpoint = *endpoint;
  }

  if (const auto* models_value = json_at(value, "modelsEndpoint"); models_value != nullptr) {
    const auto models_endpoint = json_get<std::string>(value, "modelsEndpoint");
    if (!models_endpoint.has_value() || !IsValidEndpoint(*models_endpoint)) {
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
