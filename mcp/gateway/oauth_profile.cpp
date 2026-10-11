#include "mcp/gateway/oauth_profile.h"

#include <string>
#include <utility>

#include "absl/status/status.h"

#include "mcp/client/oauth_discovery.h"
#include "mcp/validation.h"

namespace slop::mcp::gateway {

absl::StatusOr<HttpAuthConfig> ResolveOAuthAuthConfig(const ServerConfig& server, ::slop::HttpClient* http_client) {
  if (server.transport != ServerTransport::kHttp || server.auth.method != HttpAuthMethod::kOAuth) {
    return absl::InvalidArgumentError("OAuth metadata resolution requires an HTTP OAuth server");
  }
  if (!IsValidHttpUrl(server.endpoint_url, 4096, true)) {
    return absl::InvalidArgumentError("OAuth metadata discovery requires an HTTPS MCP endpoint");
  }

  HttpAuthConfig auth = server.auth;
  if (!auth.authorization_endpoint.empty() || !auth.token_endpoint.empty()) {
    const absl::Status status = ValidateHttpAuthConfig(auth);
    if (!status.ok()) return status;
    return auth;
  }

  const absl::Status discovery_config_status = ValidateHttpAuthConfigForDiscovery(auth);
  if (!discovery_config_status.ok()) return discovery_config_status;
  OAuthDiscoveryOptions options;
  options.selected_authorization_server = auth.expected_issuer;
  options.resource_metadata_url = auth.resource_metadata_url;
  auto discovery = DiscoverOAuthEndpoints(http_client, server.endpoint_url, std::move(options));
  if (!discovery.ok()) return discovery.status();
  if (!auth.expected_issuer.empty() && auth.expected_issuer != discovery->issuer) {
    return absl::PermissionDeniedError("discovered OAuth issuer does not match the configured issuer");
  }

  auth.expected_issuer = discovery->issuer;
  auth.authorization_endpoint = discovery->authorization_endpoint;
  auth.token_endpoint = discovery->token_endpoint;
  auth.resource_metadata_url = discovery->resource_metadata_url;
  auth.authorization_response_iss_parameter_supported = discovery->authorization_response_iss_parameter_supported;
  const absl::Status resolved_status = ValidateHttpAuthConfig(auth);
  if (!resolved_status.ok()) return resolved_status;
  return auth;
}

}  // namespace slop::mcp::gateway
