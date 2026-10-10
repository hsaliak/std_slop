#include "mcp/gateway/config.h"

#include <string>
#include <unordered_set>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "mcp/client/stdio_transport.h"
#include "mcp/validation.h"

namespace slop::mcp::gateway {
namespace {

constexpr std::size_t kMaxCommandBytes = 4096;
constexpr std::size_t kMaxEndpointUrlBytes = 4096;
constexpr std::size_t kMaxArgCount = 64;
constexpr std::size_t kMaxArgBytes = 4096;
constexpr std::size_t kMaxConfigBytes = 1024 * 1024;
constexpr std::size_t kMaxTraceLogPathBytes = 4096;
constexpr std::size_t kMaxTokenFilePathBytes = 4096;
constexpr std::size_t kMaxTokenEnvNameBytes = 256;
constexpr std::size_t kMaxOAuthClientIdBytes = 4096;
constexpr std::size_t kMaxOAuthUrlBytes = 4096;
constexpr std::size_t kMaxOAuthScopeCount = 64;
constexpr std::size_t kMaxOAuthScopeBytes = 256;

absl::StatusOr<std::vector<std::string>> StringArray(const nlohmann::json& value, std::size_t max_count,
                                                     std::size_t max_item_bytes, const std::string& path) {
  if (!value.is_array() || value.size() > max_count) {
    return absl::InvalidArgumentError(absl::StrCat(path, " must be a bounded array"));
  }
  std::vector<std::string> result;
  result.reserve(value.size());
  std::unordered_set<std::string> unique;
  for (const auto& item : value) {
    if (!item.is_string()) return absl::InvalidArgumentError(absl::StrCat(path, " entries must be strings"));
    const auto parsed_text = json_getter<std::string>::get(item);
    if (!parsed_text.has_value()) return absl::InvalidArgumentError(absl::StrCat(path, " entries must be strings"));
    const std::string& text = *parsed_text;
    if (text.empty() || text.size() > max_item_bytes) {
      return absl::InvalidArgumentError(absl::StrCat(path, " entries must be non-empty and bounded"));
    }
    if (!unique.insert(text).second) return absl::InvalidArgumentError(absl::StrCat(path, " entries must be unique"));
    result.push_back(text);
  }
  return result;
}

absl::StatusOr<std::string> OptionalStringField(const nlohmann::json& value, const std::string& key,
                                                std::size_t max_bytes) {
  if (json_at(value, key) == nullptr) return std::string();
  const auto text = json_get<std::string>(value, key);
  if (!text || text->empty() || text->size() > max_bytes || text->find('\0') != std::string::npos) {
    return absl::InvalidArgumentError(absl::StrCat("server auth ", key, " must be a bounded non-empty string"));
  }
  return *text;
}

absl::StatusOr<HttpAuthConfig> ParseHttpAuthConfig(const nlohmann::json* value) {
  HttpAuthConfig auth;
  if (value == nullptr) return auth;
  if (!value->is_object()) return absl::InvalidArgumentError("server auth must be an object");
  const auto type = json_get<std::string>(*value, "type");
  if (!type) return absl::InvalidArgumentError("server auth requires a string type");

  if (*type == "none") {
    const absl::Status status = ValidateJsonObjectFields(*value, {"type"}, "server auth");
    if (!status.ok()) return status;
    return auth;
  }
  if (*type == "bearer") {
    const absl::Status status = ValidateJsonObjectFields(*value, {"type", "tokenFile", "tokenEnv"}, "server auth");
    if (!status.ok()) return status;
    auth.method = HttpAuthMethod::kBearer;
    auto token_file = OptionalStringField(*value, "tokenFile", kMaxTokenFilePathBytes);
    if (!token_file.ok()) return token_file.status();
    auto token_env = OptionalStringField(*value, "tokenEnv", kMaxTokenEnvNameBytes);
    if (!token_env.ok()) return token_env.status();
    if (json_at(*value, "tokenFile") != nullptr) auth.token_file = std::move(*token_file);
    if (json_at(*value, "tokenEnv") != nullptr) auth.token_env = std::move(*token_env);
    const absl::Status config_status = ValidateHttpAuthConfig(auth);
    if (!config_status.ok()) return config_status;
    return auth;
  }
  if (*type != "oauth") return absl::InvalidArgumentError("server auth type must be none, bearer, or oauth");

  const absl::Status status =
      ValidateJsonObjectFields(*value,
                               {"type", "tokenFile", "clientId", "issuer", "scopes", "resourceMetadataUrl",
                                "authorizationEndpoint", "tokenEndpoint", "authorizationResponseIssParameterSupported"},
                               "server auth");
  if (!status.ok()) return status;
  auth.method = HttpAuthMethod::kOAuth;
  auto token_file = OptionalStringField(*value, "tokenFile", kMaxTokenFilePathBytes);
  if (!token_file.ok()) return token_file.status();
  auto client_id = OptionalStringField(*value, "clientId", kMaxOAuthClientIdBytes);
  if (!client_id.ok()) return client_id.status();
  auto issuer = OptionalStringField(*value, "issuer", kMaxOAuthUrlBytes);
  if (!issuer.ok()) return issuer.status();
  auto resource_metadata_url = OptionalStringField(*value, "resourceMetadataUrl", kMaxOAuthUrlBytes);
  if (!resource_metadata_url.ok()) return resource_metadata_url.status();
  auto authorization_endpoint = OptionalStringField(*value, "authorizationEndpoint", kMaxOAuthUrlBytes);
  if (!authorization_endpoint.ok()) return authorization_endpoint.status();
  auto token_endpoint = OptionalStringField(*value, "tokenEndpoint", kMaxOAuthUrlBytes);
  if (!token_endpoint.ok()) return token_endpoint.status();
  if (json_at(*value, "tokenFile") == nullptr) {
    return absl::InvalidArgumentError("OAuth server auth requires tokenFile");
  }
  if (json_at(*value, "clientId") == nullptr || json_at(*value, "issuer") == nullptr) {
    return absl::InvalidArgumentError("OAuth server auth requires clientId and issuer");
  }
  auth.token_file = std::move(*token_file);
  auth.client_id = std::move(*client_id);
  auth.expected_issuer = std::move(*issuer);
  auth.resource_metadata_url = std::move(*resource_metadata_url);
  auth.authorization_endpoint = std::move(*authorization_endpoint);
  auth.token_endpoint = std::move(*token_endpoint);
  if (json_at(*value, "scopes") != nullptr) {
    auto scopes =
        StringArray(*json_at(*value, "scopes"), kMaxOAuthScopeCount, kMaxOAuthScopeBytes, "server auth scopes");
    if (!scopes.ok()) return scopes.status();
    auth.scopes = std::move(*scopes);
  }
  if (json_at(*value, "authorizationResponseIssParameterSupported") != nullptr) {
    const auto supported = json_get<bool>(*value, "authorizationResponseIssParameterSupported");
    if (!supported) {
      return absl::InvalidArgumentError("server auth authorizationResponseIssParameterSupported must be a boolean");
    }
    auth.authorization_response_iss_parameter_supported = *supported;
  }
  if (!IsValidHttpUrl(auth.expected_issuer, kMaxOAuthUrlBytes) ||
      (!auth.resource_metadata_url.empty() && !IsValidHttpUrl(auth.resource_metadata_url, kMaxOAuthUrlBytes)) ||
      (!auth.authorization_endpoint.empty() && !IsValidHttpUrl(auth.authorization_endpoint, kMaxOAuthUrlBytes)) ||
      (!auth.token_endpoint.empty() && !IsValidHttpUrl(auth.token_endpoint, kMaxOAuthUrlBytes))) {
    return absl::InvalidArgumentError("OAuth server auth URLs must be valid bounded HTTP(S) URLs");
  }
  const absl::Status config_status = ValidateHttpAuthConfig(auth);
  if (!config_status.ok()) return config_status;
  return auth;
}

}  // namespace

bool IsSafeServerAlias(const std::string& alias) { return IsSafeGatewayServerAlias(alias); }

absl::StatusOr<GatewayConfig> ParseConfig(const nlohmann::json& value) {
  absl::Status root_status = ValidateJsonObjectFields(value, {"servers", "runTimeoutMs", "traceLogPath"}, "config");
  if (!root_status.ok()) return root_status;
  const nlohmann::json* servers = json_at(value, "servers");
  if (servers == nullptr || !servers->is_array() || servers->size() > kMaxServers) {
    return absl::InvalidArgumentError("config servers must be a bounded array");
  }

  GatewayConfig config;
  if (json_at(value, "runTimeoutMs") != nullptr) {
    const auto timeout = json_get<std::int64_t>(value, "runTimeoutMs");
    if (!timeout || *timeout < 1 || *timeout > 60'000) {
      return absl::InvalidArgumentError("config runTimeoutMs must be between 1 and 60000");
    }
    config.run_timeout_ms = *timeout;
  }
  if (json_at(value, "traceLogPath") != nullptr) {
    const auto trace_log_path = json_get<std::string>(value, "traceLogPath");
    if (!trace_log_path || !IsValidAbsolutePath(*trace_log_path, kMaxTraceLogPathBytes)) {
      return absl::InvalidArgumentError("config traceLogPath must be a bounded absolute path");
    }
    config.trace_log_path = *trace_log_path;
  }
  config.servers.reserve(servers->size());
  std::unordered_set<std::string> aliases;
  for (const nlohmann::json& entry : *servers) {
    absl::Status entry_status = ValidateJsonObjectFields(
        entry, {"alias", "transport", "command", "args", "endpointUrl", "auth", "allowTools"}, "server");
    if (!entry_status.ok()) return entry_status;
    const auto alias = json_get<std::string>(entry, "alias");
    const auto transport = json_get<std::string>(entry, "transport");
    const nlohmann::json* grants_value = json_at(entry, "allowTools");
    if (!alias || !transport) return absl::InvalidArgumentError("server requires alias and transport");
    if (!IsSafeServerAlias(*alias))
      return absl::InvalidArgumentError("server alias is not a safe JavaScript identifier");
    if (!aliases.insert(*alias).second) return absl::InvalidArgumentError("server aliases must be unique");

    ServerConfig server;
    server.alias = *alias;
    if (grants_value != nullptr) {
      auto parsed_grants = StringArray(*grants_value, kMaxAllowedToolsPerServer, 256, "allowTools");
      if (!parsed_grants.ok()) return parsed_grants.status();
      server.allow_tools = std::move(*parsed_grants);
    }

    if (*transport == "stdio") {
      if (json_at(entry, "auth") != nullptr) {
        return absl::InvalidArgumentError("stdio server must not include auth");
      }
      if (json_at(entry, "endpointUrl") != nullptr) {
        return absl::InvalidArgumentError("stdio server must not include endpointUrl");
      }
      const auto command = json_get<std::string>(entry, "command");
      const nlohmann::json* args_value = json_at(entry, "args");
      if (!command || args_value == nullptr) {
        return absl::InvalidArgumentError("stdio server requires command and args");
      }
      if (!IsValidAbsolutePath(*command, kMaxCommandBytes)) {
        return absl::InvalidArgumentError("stdio command must be a bounded absolute path");
      }
      auto args = StringArray(*args_value, kMaxArgCount, kMaxArgBytes, "server args");
      if (!args.ok()) return args.status();
      absl::Status process_status = mcp::ValidateStdioProcessSpec(*command, *args);
      if (!process_status.ok()) return process_status;
      server.transport = ServerTransport::kStdio;
      server.command = *command;
      server.args = std::move(*args);
    } else if (*transport == "http") {
      if (json_at(entry, "command") != nullptr || json_at(entry, "args") != nullptr) {
        return absl::InvalidArgumentError("http server must not include command or args");
      }
      const auto endpoint_url = json_get<std::string>(entry, "endpointUrl");
      if (!endpoint_url || !IsValidHttpUrl(*endpoint_url, kMaxEndpointUrlBytes)) {
        return absl::InvalidArgumentError("http server endpointUrl must be a valid bounded HTTP(S) URL");
      }
      auto auth = ParseHttpAuthConfig(json_at(entry, "auth"));
      if (!auth.ok()) return auth.status();
      if (auth->method != HttpAuthMethod::kNone && endpoint_url->rfind("https://", 0) != 0) {
        return absl::InvalidArgumentError("authenticated http servers must use HTTPS");
      }
      server.transport = ServerTransport::kHttp;
      server.endpoint_url = *endpoint_url;
      server.auth = std::move(*auth);
    } else {
      return absl::InvalidArgumentError("server transport must be stdio or http");
    }
    config.servers.push_back(std::move(server));
  }
  return config;
}

absl::StatusOr<GatewayConfig> ParseConfigText(const std::string& text) {
  if (text.size() > kMaxConfigBytes) return absl::ResourceExhaustedError("config exceeds maximum size");
  const auto parsed = json_parse(text);
  if (!parsed.has_value()) return absl::InvalidArgumentError("config is not valid JSON");
  return ParseConfig(*parsed);
}

}  // namespace slop::mcp::gateway
