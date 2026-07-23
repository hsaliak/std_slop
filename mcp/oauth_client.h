#ifndef SLOP_MCP_OAUTH_CLIENT_H_
#define SLOP_MCP_OAUTH_CLIENT_H_

#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "core/http_client.h"
#include "mcp/authorization.h"
#include "mcp/token_store.h"

namespace slop::mcp {

struct PkceAuthorizationSession {
  std::string authorization_url;
  std::string state;
  std::string code_verifier;
  std::string redirect_uri;
};

struct OAuthClientConfig {
  std::string client_id;
  std::string authorization_endpoint;
  std::string token_endpoint;
  std::vector<std::string> scopes;
  std::string redirect_uri = "http://127.0.0.1/callback";
};

absl::StatusOr<PkceAuthorizationSession> StartPkceAuthorization(const OAuthClientConfig& config);
absl::StatusOr<std::string> ExtractAuthorizationCodeFromCallback(const std::string& callback_url,
                                                                  const std::string& expected_state);
absl::StatusOr<OAuthTokenSet> ExchangeAuthorizationCode(HttpClient* http_client, const OAuthClientConfig& config,
                                                        const std::string& code,
                                                        const std::string& code_verifier);
absl::StatusOr<OAuthTokenSet> RefreshOAuthToken(HttpClient* http_client, const OAuthClientConfig& config,
                                                const std::string& refresh_token);

}  // namespace slop::mcp

#endif  // SLOP_MCP_OAUTH_CLIENT_H_
