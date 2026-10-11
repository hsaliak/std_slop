#include "mcp/gateway/oauth_cli.h"

#include <cstdlib>
#include <istream>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

#include "mcp/client/oauth_client.h"
#include "mcp/validation.h"

namespace slop::mcp::gateway {
namespace {

constexpr std::size_t kMaxCallbackUrlBytes = 8192;
constexpr std::size_t kMaxClientSecretBytes = 4096;

absl::StatusOr<std::string> ClientSecretFromEnvironment(const HttpAuthConfig& auth) {
  if (auth.client_secret_env.empty()) return std::string();
  if (!IsValidEnvironmentVariableName(auth.client_secret_env)) {
    return absl::InvalidArgumentError("OAuth client secret environment variable name is invalid");
  }
  const char* value = std::getenv(auth.client_secret_env.c_str());
  if (value == nullptr || value[0] == '\0') {
    return absl::FailedPreconditionError("OAuth client secret environment variable is unset or empty");
  }
  std::string secret(value);
  if (secret.size() > kMaxClientSecretBytes || ContainsControlCharacters(secret)) {
    return absl::InvalidArgumentError("OAuth client secret environment value is invalid");
  }
  return secret;
}

absl::StatusOr<std::string> ReadCallbackUrl(std::istream* input) {
  if (input == nullptr) return absl::InvalidArgumentError("OAuth login requires callback input");
  std::string callback;
  char character = '\0';
  while (input->get(character)) {
    if (character == '\n') break;
    if (callback.size() == kMaxCallbackUrlBytes) {
      return absl::ResourceExhaustedError("OAuth callback URL exceeds the size limit");
    }
    callback.push_back(character);
  }
  if (input->bad()) return absl::UnavailableError("Failed to read OAuth callback URL");
  if (callback.empty() && input->eof()) return absl::InvalidArgumentError("OAuth callback URL is required");
  if (!callback.empty() && callback.back() == '\r') callback.pop_back();
  return callback;
}

absl::StatusOr<const ServerConfig*> FindOAuthServer(const GatewayConfig& config, const std::string& alias) {
  for (const ServerConfig& server : config.servers) {
    if (server.alias != alias) continue;
    if (server.transport != ServerTransport::kHttp) {
      return absl::InvalidArgumentError("OAuth setup requires an HTTP gateway server");
    }
    if (server.auth.method != HttpAuthMethod::kOAuth) {
      return absl::InvalidArgumentError("OAuth setup requires an OAuth auth profile");
    }
    if (!IsValidHttpUrl(server.endpoint_url, 4096, true)) {
      return absl::InvalidArgumentError("OAuth setup requires an HTTPS MCP endpoint");
    }
    const absl::Status auth_status = ValidateHttpAuthConfig(server.auth);
    if (!auth_status.ok()) return auth_status;
    return &server;
  }
  return absl::NotFoundError(absl::StrCat("Gateway server not found: ", alias));
}

absl::StatusOr<::slop::mcp::OAuthClientConfig> OAuthConfigFromServer(const ServerConfig& server) {
  auto client_secret = ClientSecretFromEnvironment(server.auth);
  if (!client_secret.ok()) return client_secret.status();
  ::slop::mcp::OAuthClientConfig oauth;
  oauth.client_id = server.auth.client_id;
  oauth.client_secret = std::move(*client_secret);
  oauth.authorization_endpoint = server.auth.authorization_endpoint;
  oauth.token_endpoint = server.auth.token_endpoint;
  oauth.scopes = server.auth.scopes;
  oauth.resource = server.endpoint_url;
  oauth.issuer = server.auth.expected_issuer;
  oauth.authorization_response_iss_parameter_supported = server.auth.authorization_response_iss_parameter_supported;
  return oauth;
}

}  // namespace

absl::StatusOr<OAuthCliOptions> ParseOAuthCliOptions(const std::vector<std::string>& args) {
  if (args.size() < 2 || args[0] != "auth") {
    return absl::InvalidArgumentError("usage: run_js_server auth login|refresh --config PATH --server ALIAS");
  }
  OAuthCliOptions options;
  if (args[1] == "login") {
    options.action = OAuthCliAction::kLogin;
  } else if (args[1] == "refresh") {
    options.action = OAuthCliAction::kRefresh;
  } else {
    return absl::InvalidArgumentError("OAuth command must be login or refresh");
  }

  bool saw_config = false;
  bool saw_server = false;
  for (std::size_t i = 2; i < args.size();) {
    const std::string& flag = args[i];
    if (flag != "--config" && flag != "--server") {
      return absl::InvalidArgumentError("unsupported OAuth command argument");
    }
    if (i + 1 >= args.size() || args[i + 1].empty() || args[i + 1].rfind("--", 0) == 0) {
      return absl::InvalidArgumentError(absl::StrCat("OAuth command argument requires a value: ", flag));
    }
    if (flag == "--config") {
      if (saw_config) return absl::InvalidArgumentError("OAuth command has duplicate --config");
      saw_config = true;
      options.config_path = args[i + 1];
    } else {
      if (saw_server) return absl::InvalidArgumentError("OAuth command has duplicate --server");
      saw_server = true;
      options.server_alias = args[i + 1];
    }
    i += 2;
  }
  if (!saw_config || !saw_server) {
    return absl::InvalidArgumentError("usage: run_js_server auth login|refresh --config PATH --server ALIAS");
  }
  if (!IsSafeServerAlias(options.server_alias)) {
    return absl::InvalidArgumentError("OAuth server alias is not valid");
  }
  return options;
}

absl::Status RunOAuthCliCommand(const OAuthCliOptions& options, const GatewayConfig& config,
                                ::slop::HttpClient* http_client, std::istream* input, std::ostream* output) {
  if (http_client == nullptr || output == nullptr) {
    return absl::InvalidArgumentError("OAuth command requires HTTP client and output stream");
  }
  if (options.action != OAuthCliAction::kLogin && options.action != OAuthCliAction::kRefresh) {
    return absl::InvalidArgumentError("OAuth command action is invalid");
  }
  if (!IsSafeServerAlias(options.server_alias)) {
    return absl::InvalidArgumentError("OAuth server alias is not valid");
  }
  auto server = FindOAuthServer(config, options.server_alias);
  if (!server.ok()) return server.status();
  auto oauth = OAuthConfigFromServer(**server);
  if (!oauth.ok()) return oauth.status();

  if (options.action == OAuthCliAction::kLogin) {
    auto session = ::slop::mcp::StartPkceAuthorization(*oauth);
    if (!session.ok()) return session.status();
    *output << "Open this URL in a browser and complete authorization:\n"
            << session->authorization_url << "\nPaste callback URL: " << std::flush;
    auto callback = ReadCallbackUrl(input);
    if (!callback.ok()) return callback.status();
    const absl::Status status = ::slop::mcp::CompletePkceAuthorizationAndSaveTokens(
        http_client, *oauth, *session, *callback, (*server)->auth.token_file);
    if (!status.ok()) return status;
    *output << "OAuth login complete for gateway server: " << options.server_alias << "\n";
    return absl::OkStatus();
  }

  const absl::Status status = ::slop::mcp::RefreshOAuthTokenFile(http_client, *oauth, (*server)->auth.token_file);
  if (!status.ok()) return status;
  *output << "OAuth token refreshed for gateway server: " << options.server_alias << "\n";
  return absl::OkStatus();
}

}  // namespace slop::mcp::gateway
