#ifndef SLOP_MCP_GATEWAY_OAUTH_PROFILE_H_
#define SLOP_MCP_GATEWAY_OAUTH_PROFILE_H_

#include "absl/status/statusor.h"

#include "core/http_client.h"
#include "mcp/client/auth_config.h"
#include "mcp/gateway/config.h"

namespace slop::mcp::gateway {

// Resolves OAuth metadata before gateway login, refresh, or HTTP client startup.
absl::StatusOr<HttpAuthConfig> ResolveOAuthAuthConfig(const ServerConfig& server, ::slop::HttpClient* http_client);

}  // namespace slop::mcp::gateway

#endif  // SLOP_MCP_GATEWAY_OAUTH_PROFILE_H_
