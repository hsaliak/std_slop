#include "mcp/client/auth_config.h"

#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "mcp/client/token_store.h"

namespace slop::mcp {
namespace {

bool HasControlCharacter(const std::string& value) {
  for (const char c : value) {
    const unsigned char ch = static_cast<unsigned char>(c);
    if (ch < 0x20 || ch == 0x7f) return true;
  }
  return false;
}

absl::Status ValidateTokenSource(const HttpAuthConfig& config) {
  const bool has_file = !config.token_file.empty();
  const bool has_env = !config.token_env.empty();
  if (has_file == has_env) {
    return absl::InvalidArgumentError("HTTP MCP auth requires exactly one token file or environment variable");
  }
  if (HasControlCharacter(config.token_file) || HasControlCharacter(config.token_env)) {
    return absl::InvalidArgumentError("HTTP MCP auth token source contains a control character");
  }
  if (has_env && (config.token_env.find('=') != std::string::npos || config.token_env.front() == '=')) {
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
      if (!config.token_file.empty() || !config.token_env.empty() || !config.expected_issuer.empty()) {
        return absl::InvalidArgumentError("Unauthenticated HTTP MCP config cannot contain token settings");
      }
      return absl::OkStatus();
    case HttpAuthMethod::kBearer:
      if (!config.expected_issuer.empty()) {
        return absl::InvalidArgumentError("Bearer HTTP MCP config cannot contain an OAuth issuer");
      }
      return ValidateTokenSource(config);
    case HttpAuthMethod::kOAuth:
      if (config.token_file.empty() || !config.token_env.empty() || config.expected_issuer.empty()) {
        return absl::InvalidArgumentError("OAuth HTTP MCP config requires a token file and expected issuer");
      }
      if (HasControlCharacter(config.token_file) || HasControlCharacter(config.expected_issuer)) {
        return absl::InvalidArgumentError("OAuth HTTP MCP config contains a control character");
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
