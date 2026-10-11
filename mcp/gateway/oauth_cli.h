#ifndef SLOP_MCP_GATEWAY_OAUTH_CLI_H_
#define SLOP_MCP_GATEWAY_OAUTH_CLI_H_

#include <iosfwd>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "core/http_client.h"
#include "mcp/gateway/config.h"

namespace slop::mcp::gateway {

enum class OAuthCliAction { kLogin, kRefresh };

struct OAuthCliOptions {
  OAuthCliAction action = OAuthCliAction::kLogin;
  std::string config_path;
  std::string server_alias;
};

absl::StatusOr<OAuthCliOptions> ParseOAuthCliOptions(const std::vector<std::string>& args);
absl::Status RunOAuthCliCommand(const OAuthCliOptions& options, const GatewayConfig& config,
                                slop::HttpClient* http_client, std::istream* input, std::ostream* output);

}  // namespace slop::mcp::gateway

#endif  // SLOP_MCP_GATEWAY_OAUTH_CLI_H_
