#ifndef SLOP_MCP_DECISION_API_CONFIG_H_
#define SLOP_MCP_DECISION_API_CONFIG_H_

#include <cstdint>
#include <optional>
#include <string>

#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

namespace slop::mcp::decision_api {

inline constexpr char kDefaultEndpoint[] = "https://openrouter.ai/api/alpha/decisions";
inline constexpr char kDefaultModelsEndpoint[] = "https://openrouter.ai/api/v1/models?output_modalities=decisions";

struct Config {
  std::string api_key;
  std::string model;
  std::string endpoint = kDefaultEndpoint;
  std::optional<std::string> models_endpoint = kDefaultModelsEndpoint;
  std::int64_t timeout_ms = 30000;
};

absl::StatusOr<Config> ParseConfig(const nlohmann::json& value);
absl::StatusOr<Config> ParseConfigText(const std::string& text);

}  // namespace slop::mcp::decision_api

#endif  // SLOP_MCP_DECISION_API_CONFIG_H_
