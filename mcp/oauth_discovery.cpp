#include "mcp/oauth_discovery.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "mcp/authorization.h"

namespace slop::mcp {
namespace {

bool IsHttpUrl(const std::string& url) { return absl::StartsWith(url, "https://") || absl::StartsWith(url, "http://"); }

bool IsHttpsUrl(const std::string& url) { return absl::StartsWith(url, "https://"); }

std::string TrimTrailingSlash(std::string value) {
  while (!value.empty() && value.back() == '/') value.pop_back();
  return value;
}

std::string AuthorizationServerMetadataUrl(const std::string& authorization_server_url) {
  if (authorization_server_url.find("/.well-known/") != std::string::npos) return authorization_server_url;
  const std::string trimmed = TrimTrailingSlash(authorization_server_url);
  const size_t authority_start = trimmed.find("://");
  if (authority_start == std::string::npos) return absl::StrCat(trimmed, "/.well-known/oauth-authorization-server");
  const size_t path_start = trimmed.find('/', authority_start + 3);
  if (path_start == std::string::npos) return absl::StrCat(trimmed, "/.well-known/oauth-authorization-server");
  return absl::StrCat(trimmed.substr(0, path_start), "/.well-known/oauth-authorization-server",
                      trimmed.substr(path_start));
}

absl::StatusOr<nlohmann::json> GetJson(HttpClient* http_client, const std::string& url, const std::string& label) {
  auto body = http_client->GetOnce(url, {"Accept: application/json"});
  if (!body.ok()) return body.status();
  auto parsed = json_parse(*body);
  if (!parsed.has_value() || parsed->is_discarded()) {
    return absl::InvalidArgumentError(absl::StrCat(label, " returned malformed JSON"));
  }
  return *parsed;
}

}  // namespace

absl::StatusOr<OAuthDiscoveryResult> DiscoverOAuthEndpoints(HttpClient* http_client,
                                                            const std::string& mcp_endpoint_url,
                                                            OAuthDiscoveryOptions options) {
  if (http_client == nullptr) return absl::InvalidArgumentError("http_client must not be null");
  if (!IsHttpUrl(mcp_endpoint_url)) return absl::InvalidArgumentError("MCP endpoint URL must use http or https");

  const nlohmann::json probe = {
      {"jsonrpc", "2.0"},
      {"id", "oauth-discovery"},
      {"method", "initialize"},
      {"params",
       {{"protocolVersion", "2025-06-18"},
        {"capabilities", nlohmann::json::object()},
        {"clientInfo", {{"name", "std_slop"}, {"version", "oauth-discovery"}}}}},
  };
  auto challenge = http_client->PostOnceWithResponse(
      mcp_endpoint_url, json_dump(probe),
      {"Accept: application/json, text/event-stream", "Content-Type: application/json",
       "MCP-Protocol-Version: 2025-06-18", "Mcp-Method: initialize"});
  if (!challenge.ok()) return challenge.status();
  if (challenge->status_code != 401) {
    return absl::UnauthenticatedError(
        "MCP OAuth discovery requires a 401 WWW-Authenticate resource_metadata challenge; pass "
        "--authorization-endpoint and --token-endpoint manually");
  }

  const auto header = challenge->headers.find("www-authenticate");
  if (header == challenge->headers.end())
    return absl::UnauthenticatedError("MCP OAuth discovery missing WWW-Authenticate header");
  auto resource_metadata_url = ParseWwwAuthenticateResourceMetadata(header->second);
  if (!resource_metadata_url.ok()) return resource_metadata_url.status();
  if (!IsHttpsUrl(*resource_metadata_url)) {
    return absl::InvalidArgumentError("MCP OAuth resource metadata URL must use https");
  }

  auto resource_json = GetJson(http_client, *resource_metadata_url, "protected resource metadata");
  if (!resource_json.ok()) return resource_json.status();
  auto resource_metadata = ParseProtectedResourceMetadata(*resource_json);
  if (!resource_metadata.ok()) return resource_metadata.status();
  std::string authorization_server_url = options.selected_authorization_server;
  if (authorization_server_url.empty()) {
    if (resource_metadata->authorization_servers.size() != 1) {
      return absl::FailedPreconditionError("multiple authorization servers advertised; select one explicitly");
    }
    authorization_server_url = resource_metadata->authorization_servers.front();
  }
  if (!IsHttpsUrl(authorization_server_url)) {
    return absl::InvalidArgumentError("MCP OAuth authorization server URL must use https");
  }
  const std::string metadata_url = AuthorizationServerMetadataUrl(authorization_server_url);
  auto server_json = GetJson(http_client, metadata_url, "authorization server metadata");
  if (!server_json.ok()) return server_json.status();
  auto server_metadata = ParseAuthorizationServerMetadata(*server_json);
  if (!server_metadata.ok()) return server_metadata.status();
  if (!IsHttpsUrl(server_metadata->authorization_endpoint) || !IsHttpsUrl(server_metadata->token_endpoint)) {
    return absl::InvalidArgumentError("MCP OAuth authorization and token endpoints must use https");
  }
  const absl::Status binding =
      ValidateAuthorizationBinding(*resource_metadata, mcp_endpoint_url, *server_metadata, authorization_server_url);
  if (!binding.ok()) return binding;

  OAuthDiscoveryResult result;
  result.resource_metadata_url = *resource_metadata_url;
  result.resource = resource_metadata->resource;
  result.issuer = server_metadata->issuer;
  result.authorization_server_url = authorization_server_url;
  result.authorization_endpoint = server_metadata->authorization_endpoint;
  result.token_endpoint = server_metadata->token_endpoint;
  result.scopes_supported = server_metadata->scopes_supported;
  result.authorization_response_iss_parameter_supported =
      server_metadata->authorization_response_iss_parameter_supported;
  return result;
}

}  // namespace slop::mcp
