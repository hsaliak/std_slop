#ifndef SLOP_MCP_OAUTH_DISCOVERY_H_
#define SLOP_MCP_OAUTH_DISCOVERY_H_

#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "core/http_client.h"

namespace slop::mcp {

struct OAuthDiscoveryResult {
  std::string resource_metadata_url;
  std::string authorization_server_url;
  std::string authorization_endpoint;
  std::string token_endpoint;
  std::vector<std::string> scopes_supported;
};

absl::StatusOr<OAuthDiscoveryResult> DiscoverOAuthEndpoints(HttpClient* http_client,
                                                            const std::string& mcp_endpoint_url);

}  // namespace slop::mcp

#endif  // SLOP_MCP_OAUTH_DISCOVERY_H_
