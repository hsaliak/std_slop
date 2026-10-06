#ifndef SLOP_MCP_GATEWAY_CONFIG_H_
#define SLOP_MCP_GATEWAY_CONFIG_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

namespace slop::mcp::gateway {

struct ServerConfig {
  std::string alias;
  std::string command;
  std::vector<std::string> args;
  std::vector<std::string> allow_tools;
};

struct GatewayConfig {
  std::vector<ServerConfig> servers;
  std::int64_t run_timeout_ms = 30'000;
};

inline constexpr std::size_t kMaxServers = 8;
inline constexpr std::size_t kMaxAllowedToolsPerServer = 1000;

bool IsSafeServerAlias(const std::string& alias);

absl::StatusOr<GatewayConfig> ParseConfig(const nlohmann::json& value);
absl::StatusOr<GatewayConfig> ParseConfigText(const std::string& text);

}  // namespace slop::mcp::gateway

#endif  // SLOP_MCP_GATEWAY_CONFIG_H_
