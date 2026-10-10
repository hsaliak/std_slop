#include "mcp/client/auth_config.h"

#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "mcp/client/token_store.h"
#include "mcp/validation.h"

namespace slop::mcp {
namespace {

bool HasOAuthProfileFields(const HttpAuthConfig& config) {
  return !config.client_id.empty() || !config.scopes.empty() || !config.resource_metadata_url.empty() ||
         !config.authorization_endpoint.empty() || !config.token_endpoint.empty() ||
         config.authorization_response_iss_parameter_supported;
}

absl::Status ValidateTokenSource(const HttpAuthConfig& config) {
  const bool has_file = !config.token_file.empty();
  const bool has_env = !config.token_env.empty();
  if (has_file == has_env) {
    return absl::InvalidArgumentError("HTTP MCP auth requires exactly one token file or environment variable");
  }
  if (has_file) {
    if (!IsValidAbsolutePath(config.token_file, 4096)) {
      return absl::InvalidArgumentError("HTTP MCP token file must be a bounded absolute path");
    }
    return absl::OkStatus();
  }
  if (!IsValidEnvironmentVariableName(config.token_env)) {
    return absl::InvalidArgumentError("HTTP MCP auth environment variable name is invalid");
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> LoadBearerToken(const HttpAuthConfig& config) {
  if (!config.token_env.empty()) {
    const char* token = std::getenv(config.token_env.c_str());
    if (token == nullptr) return absl::UnauthenticatedError("HTTP MCP bearer token environment variable is unset");
    const absl::Status token_status = ValidateBearerTokenForHttpHeader(token);
    if (!token_status.ok()) return absl::UnauthenticatedError("HTTP MCP bearer token environment value is invalid");
    return std::string(token);
  }

  auto tokens = LoadOAuthTokens(config.token_file);
  if (!tokens.ok()) return absl::UnauthenticatedError("HTTP MCP bearer token file is missing or invalid");
  return tokens->access_token;
}

}  // namespace

absl::Status ValidateHttpAuthConfig(const HttpAuthConfig& config) {
  switch (config.method) {
    case HttpAuthMethod::kNone:
      if (!config.token_file.empty() || !config.token_env.empty() || !config.expected_issuer.empty() ||
          HasOAuthProfileFields(config)) {
        return absl::InvalidArgumentError("Unauthenticated HTTP MCP config cannot contain auth settings");
      }
      return absl::OkStatus();
    case HttpAuthMethod::kBearer:
      if (!config.expected_issuer.empty() || HasOAuthProfileFields(config)) {
        return absl::InvalidArgumentError("Bearer HTTP MCP config cannot contain OAuth profile settings");
      }
      return ValidateTokenSource(config);
    case HttpAuthMethod::kOAuth:
      if (config.token_file.empty() || !config.token_env.empty() || config.expected_issuer.empty() ||
          config.client_id.empty()) {
        return absl::InvalidArgumentError("OAuth HTTP MCP config requires token file, issuer, and client ID");
      }
      if (!IsValidAbsolutePath(config.token_file, 4096)) {
        return absl::InvalidArgumentError("HTTP MCP token file must be a bounded absolute path");
      }
      if (ContainsControlCharacters(config.expected_issuer) || ContainsControlCharacters(config.client_id) ||
          ContainsControlCharacters(config.resource_metadata_url) ||
          ContainsControlCharacters(config.authorization_endpoint) ||
          ContainsControlCharacters(config.token_endpoint)) {
        return absl::InvalidArgumentError("OAuth HTTP MCP config contains a control character");
      }
      if (!IsValidHttpUrl(config.expected_issuer, 4096, true) ||
          (!config.resource_metadata_url.empty() && !IsValidHttpUrl(config.resource_metadata_url, 4096, true))) {
        return absl::InvalidArgumentError("OAuth HTTP MCP issuer and resource metadata URL must use HTTPS");
      }
      if (config.authorization_endpoint.empty() != config.token_endpoint.empty() ||
          (!config.authorization_endpoint.empty() && (!IsValidHttpUrl(config.authorization_endpoint, 4096, true) ||
                                                      !IsValidHttpUrl(config.token_endpoint, 4096, true)))) {
        return absl::InvalidArgumentError("OAuth HTTP MCP authorization and token endpoints must both use HTTPS");
      }
      if (config.scopes.size() > 64) return absl::InvalidArgumentError("OAuth HTTP MCP scopes exceed the limit");
      for (const std::string& scope : config.scopes) {
        if (scope.empty() || scope.size() > 256 || scope.find(' ') != std::string::npos ||
            ContainsControlCharacters(scope)) {
          return absl::InvalidArgumentError("OAuth HTTP MCP scopes must be bounded single tokens");
        }
      }
      return absl::OkStatus();
  }
  return absl::InvalidArgumentError("HTTP MCP auth method is invalid");
}

absl::StatusOr<std::optional<std::string>> ResolveHttpAuthToken(const std::string& resource_url,
                                                                const HttpAuthConfig& config) {
  const absl::Status config_status = ValidateHttpAuthConfig(config);
  if (!config_status.ok()) return config_status;
  if (config.method == HttpAuthMethod::kNone) return std::nullopt;

  if (config.method == HttpAuthMethod::kBearer) {
    auto token = LoadBearerToken(config);
    if (!token.ok()) return token.status();
    return std::optional<std::string>(std::move(*token));
  }

  auto tokens = LoadOAuthTokens(config.token_file);
  if (!tokens.ok()) return absl::UnauthenticatedError("HTTP MCP OAuth token file is missing or invalid");
  if (tokens->issuer.empty() || tokens->resource.empty() || tokens->issuer != config.expected_issuer ||
      tokens->resource != resource_url) {
    return absl::PermissionDeniedError("HTTP MCP OAuth token binding does not match the configured server");
  }
  if (tokens->expires_at_unix_seconds != 0 && tokens->expires_at_unix_seconds <= std::time(nullptr)) {
    return absl::UnauthenticatedError("HTTP MCP OAuth token is expired");
  }
  return std::optional<std::string>(std::move(tokens->access_token));
}

}  // namespace slop::mcp
