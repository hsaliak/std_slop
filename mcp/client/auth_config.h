#ifndef SLOP_MCP_CLIENT_AUTH_CONFIG_H_
#define SLOP_MCP_CLIENT_AUTH_CONFIG_H_

#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace slop::mcp {

enum class HttpAuthMethod {
  kNone,
  kBearer,
  kOAuth,
};

// Describes how an HTTP MCP client obtains its bearer credential. It contains
// references to secrets, not the secret values themselves.
struct HttpAuthConfig {
  HttpAuthMethod method = HttpAuthMethod::kNone;
  std::string token_file;
  std::string token_env;
  std::string expected_issuer;
  std::string client_id;
  std::string client_secret_env;
  std::vector<std::string> scopes;
  std::string resource_metadata_url;
  std::string authorization_endpoint;
  std::string token_endpoint;
  bool authorization_response_iss_parameter_supported = false;
};

absl::Status ValidateHttpAuthConfig(const HttpAuthConfig& config);

// Resolves the configured credential. OAuth tokens are checked against both
// the expected issuer and the resource URL before they can be used.
absl::StatusOr<std::optional<std::string>> ResolveHttpAuthToken(const std::string& resource_url,
                                                                const HttpAuthConfig& config);

}  // namespace slop::mcp

#endif  // SLOP_MCP_CLIENT_AUTH_CONFIG_H_
