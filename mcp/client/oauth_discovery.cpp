#include "mcp/client/oauth_discovery.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"

#include "core/json_utils.h"
#include "mcp/client/authorization.h"
#include "mcp/validation.h"

namespace slop::mcp {
namespace {

std::vector<std::string> ProtectedResourceMetadataUrls(const std::string& resource_url) {
  const absl::string_view resource_url_view(resource_url);
  const size_t scheme_end = resource_url_view.find("://");
  const size_t authority_start = scheme_end + 3;
  size_t suffix_start = resource_url_view.find_first_of("?#", authority_start);
  if (suffix_start == absl::string_view::npos) suffix_start = resource_url_view.size();
  const size_t path_start = resource_url_view.find('/', authority_start);
  const bool has_path = path_start != absl::string_view::npos && path_start < suffix_start;
  const size_t authority_end = has_path ? path_start : suffix_start;
  const absl::string_view authority = resource_url_view.substr(0, authority_end);
  absl::string_view path = has_path ? resource_url_view.substr(path_start, suffix_start - path_start) : "";
  while (absl::EndsWith(path, "/")) path = absl::StripSuffix(path, "/");

  const size_t query_start = resource_url_view.find('?', authority_start);
  const size_t fragment_start = resource_url_view.find('#', authority_start);
  absl::string_view query;
  if (query_start != absl::string_view::npos &&
      (fragment_start == absl::string_view::npos || query_start < fragment_start)) {
    query =
        resource_url_view.substr(query_start, fragment_start == absl::string_view::npos ? absl::string_view::npos
                                                                                        : fragment_start - query_start);
  }

  const std::string root_url = absl::StrCat(authority, "/.well-known/oauth-protected-resource");
  const std::string path_url = absl::StrCat(root_url, path, query);
  if (path_url == root_url) return {root_url};
  return {path_url, root_url};
}

std::vector<std::string> AuthorizationServerMetadataUrls(const std::string& issuer) {
  absl::string_view trimmed(issuer);
  while (absl::EndsWith(trimmed, "/")) trimmed = absl::StripSuffix(trimmed, "/");
  const size_t authority_start = trimmed.find("://") + 3;
  const size_t path_start = trimmed.find('/', authority_start);
  if (path_start == absl::string_view::npos) {
    return {absl::StrCat(trimmed, "/.well-known/oauth-authorization-server"),
            absl::StrCat(trimmed, "/.well-known/openid-configuration")};
  }

  const absl::string_view authority = trimmed.substr(0, path_start);
  const absl::string_view path = trimmed.substr(path_start);
  return {absl::StrCat(authority, "/.well-known/oauth-authorization-server", path),
          absl::StrCat(authority, "/.well-known/openid-configuration", path),
          absl::StrCat(trimmed, "/.well-known/openid-configuration")};
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

absl::StatusOr<std::pair<std::string, ProtectedResourceMetadata>> FetchProtectedResourceMetadata(
    HttpClient* http_client, const std::string& resource_url, const std::vector<std::string>& metadata_urls) {
  absl::Status last_status = absl::NotFoundError("protected resource metadata was not found");
  for (const std::string& metadata_url : metadata_urls) {
    auto json = GetJson(http_client, metadata_url, "protected resource metadata");
    if (!json.ok()) {
      last_status = json.status();
      continue;
    }
    auto metadata = ParseProtectedResourceMetadata(*json);
    if (!metadata.ok()) {
      last_status = metadata.status();
      continue;
    }
    if (metadata->resource != resource_url) {
      last_status = absl::PermissionDeniedError("protected resource metadata resource mismatch");
      continue;
    }
    return std::make_pair(metadata_url, std::move(*metadata));
  }
  return last_status;
}

absl::StatusOr<AuthorizationServerMetadata> FetchAuthorizationServerMetadata(
    HttpClient* http_client, const ProtectedResourceMetadata& resource_metadata, const std::string& resource_url,
    const std::string& issuer, const std::vector<std::string>& metadata_urls) {
  absl::Status last_status = absl::NotFoundError("authorization server metadata was not found");
  for (const std::string& metadata_url : metadata_urls) {
    auto json = GetJson(http_client, metadata_url, "authorization server metadata");
    if (!json.ok()) {
      last_status = json.status();
      continue;
    }
    auto metadata = ParseAuthorizationServerMetadata(*json);
    if (!metadata.ok()) {
      last_status = metadata.status();
      continue;
    }
    if (!IsValidHttpUrl(metadata->authorization_endpoint, 4096, true) ||
        !IsValidHttpUrl(metadata->token_endpoint, 4096, true)) {
      last_status = absl::InvalidArgumentError("MCP OAuth authorization and token endpoints must use https");
      continue;
    }
    const absl::Status binding = ValidateAuthorizationBinding(resource_metadata, resource_url, *metadata, issuer);
    if (!binding.ok()) {
      last_status = binding;
      continue;
    }
    return std::move(*metadata);
  }
  return last_status;
}

}  // namespace

absl::StatusOr<OAuthDiscoveryResult> DiscoverOAuthEndpoints(HttpClient* http_client,
                                                            const std::string& mcp_endpoint_url,
                                                            OAuthDiscoveryOptions options) {
  if (http_client == nullptr) return absl::InvalidArgumentError("http_client must not be null");
  if (!IsValidHttpUrl(mcp_endpoint_url, 4096)) {
    return absl::InvalidArgumentError("MCP endpoint URL must be a bounded HTTP(S) URL");
  }

  std::vector<std::string> resource_metadata_urls;
  if (!options.resource_metadata_url.empty()) {
    if (!IsValidHttpUrl(options.resource_metadata_url, 4096, true)) {
      return absl::InvalidArgumentError("MCP OAuth resource metadata URL must use https");
    }
    resource_metadata_urls.push_back(options.resource_metadata_url);
  } else {
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
    if (challenge.ok() && challenge->status_code == 401) {
      const auto header = challenge->headers.find("www-authenticate");
      if (header != challenge->headers.end()) {
        auto discovered_url = ParseWwwAuthenticateResourceMetadata(header->second);
        if (discovered_url.ok()) {
          if (!IsValidHttpUrl(*discovered_url, 4096, true)) {
            return absl::InvalidArgumentError("MCP OAuth resource metadata URL must use https");
          }
          resource_metadata_urls.push_back(std::move(*discovered_url));
        }
      }
    }
    if (resource_metadata_urls.empty()) {
      resource_metadata_urls = ProtectedResourceMetadataUrls(mcp_endpoint_url);
    }
  }

  auto resource_metadata_result = FetchProtectedResourceMetadata(http_client, mcp_endpoint_url, resource_metadata_urls);
  if (!resource_metadata_result.ok()) return resource_metadata_result.status();
  std::string resource_metadata_url = std::move(resource_metadata_result->first);
  ProtectedResourceMetadata resource_metadata = std::move(resource_metadata_result->second);

  std::string authorization_server_url = options.selected_authorization_server;
  if (authorization_server_url.empty()) {
    if (resource_metadata.authorization_servers.size() != 1) {
      return absl::FailedPreconditionError("multiple authorization servers advertised; select one explicitly");
    }
    authorization_server_url = resource_metadata.authorization_servers.front();
  }
  if (!IsValidHttpUrl(authorization_server_url, 4096, true) ||
      authorization_server_url.find_first_of("?#") != std::string::npos) {
    return absl::InvalidArgumentError("MCP OAuth authorization server URL must be an HTTPS issuer URL");
  }
  if (std::find(resource_metadata.authorization_servers.begin(), resource_metadata.authorization_servers.end(),
                authorization_server_url) == resource_metadata.authorization_servers.end()) {
    return absl::PermissionDeniedError("authorization server was not advertised by the resource");
  }

  const std::vector<std::string> authorization_metadata_urls =
      AuthorizationServerMetadataUrls(authorization_server_url);
  auto server_metadata = FetchAuthorizationServerMetadata(http_client, resource_metadata, mcp_endpoint_url,
                                                          authorization_server_url, authorization_metadata_urls);
  if (!server_metadata.ok()) return server_metadata.status();

  OAuthDiscoveryResult result;
  result.resource_metadata_url = std::move(resource_metadata_url);
  result.resource = resource_metadata.resource;
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
