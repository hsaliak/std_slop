#include "mcp/authorization.h"

#include <algorithm>
#include <string>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"

namespace slop::mcp {
namespace {

std::string Trim(absl::string_view value) { return std::string(absl::StripAsciiWhitespace(value)); }

absl::StatusOr<std::vector<std::string>> GetOptionalStringArray(const nlohmann::json& metadata,
                                                                const std::string& key) {
  const auto* value = json_at(metadata, key);
  if (value == nullptr) return std::vector<std::string>{};
  auto parsed = json_get<std::vector<std::string>>(metadata, key);
  if (!parsed) return absl::InvalidArgumentError(absl::StrCat(key, " must be an array of strings"));
  return *parsed;
}

std::string Unquote(absl::string_view raw) {
  raw = absl::StripAsciiWhitespace(raw);
  if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') return std::string(raw);
  std::string out;
  bool escaping = false;
  for (size_t i = 1; i + 1 < raw.size(); ++i) {
    const char c = raw[i];
    if (escaping) {
      out.push_back(c);
      escaping = false;
    } else if (c == '\\') {
      escaping = true;
    } else {
      out.push_back(c);
    }
  }
  if (escaping) out.push_back('\\');
  return out;
}

}  // namespace

absl::StatusOr<std::string> ParseWwwAuthenticateResourceMetadata(absl::string_view header) {
  header = absl::StripAsciiWhitespace(header);
  if (header.empty()) return absl::InvalidArgumentError("WWW-Authenticate header is empty");

  const size_t first_space = header.find(' ');
  const absl::string_view scheme = first_space == absl::string_view::npos ? header : header.substr(0, first_space);
  if (!absl::EqualsIgnoreCase(scheme, "Bearer")) {
    return absl::InvalidArgumentError("WWW-Authenticate scheme must be Bearer");
  }
  if (first_space == absl::string_view::npos) {
    return absl::UnauthenticatedError("WWW-Authenticate missing resource_metadata");
  }

  absl::string_view params = header.substr(first_space + 1);
  while (!params.empty()) {
    params = absl::StripLeadingAsciiWhitespace(params);
    if (absl::StartsWith(params, ",")) {
      params.remove_prefix(1);
      continue;
    }
    const size_t equals = params.find('=');
    if (equals == absl::string_view::npos) break;
    const std::string key = Trim(params.substr(0, equals));
    params.remove_prefix(equals + 1);

    std::string value;
    if (!params.empty() && params.front() == '"') {
      bool escaping = false;
      size_t end = 1;
      for (; end < params.size(); ++end) {
        const char c = params[end];
        if (escaping) {
          escaping = false;
        } else if (c == '\\') {
          escaping = true;
        } else if (c == '"') {
          break;
        }
      }
      if (end >= params.size()) return absl::InvalidArgumentError("unterminated quoted WWW-Authenticate value");
      value = Unquote(params.substr(0, end + 1));
      params.remove_prefix(end + 1);
    } else {
      const size_t comma = params.find(',');
      value = Trim(comma == absl::string_view::npos ? params : params.substr(0, comma));
      if (comma == absl::string_view::npos) {
        params = absl::string_view();
      } else {
        params.remove_prefix(comma + 1);
      }
    }

    if (key == "resource_metadata") {
      if (value.empty()) return absl::InvalidArgumentError("resource_metadata must not be empty");
      return value;
    }
    if (!params.empty() && params.front() == ',') params.remove_prefix(1);
  }

  return absl::UnauthenticatedError("WWW-Authenticate missing resource_metadata");
}

absl::StatusOr<ProtectedResourceMetadata> ParseProtectedResourceMetadata(const nlohmann::json& metadata) {
  if (!metadata.is_object()) return absl::InvalidArgumentError("protected resource metadata must be an object");
  ProtectedResourceMetadata parsed;
  parsed.resource = json_get_or(metadata, "resource", std::string{});
  if (parsed.resource.empty()) return absl::InvalidArgumentError("protected resource metadata missing resource");
  auto authorization_servers = GetOptionalStringArray(metadata, "authorization_servers");
  if (!authorization_servers.ok()) return authorization_servers.status();
  parsed.authorization_servers = *authorization_servers;
  if (parsed.authorization_servers.empty()) {
    return absl::InvalidArgumentError("protected resource metadata missing authorization_servers");
  }
  auto scopes_supported = GetOptionalStringArray(metadata, "scopes_supported");
  if (!scopes_supported.ok()) return scopes_supported.status();
  parsed.scopes_supported = *scopes_supported;
  return parsed;
}

absl::StatusOr<AuthorizationServerMetadata> ParseAuthorizationServerMetadata(const nlohmann::json& metadata) {
  if (!metadata.is_object()) return absl::InvalidArgumentError("authorization server metadata must be an object");
  AuthorizationServerMetadata parsed;
  parsed.issuer = json_get_or(metadata, "issuer", std::string{});
  parsed.authorization_endpoint = json_get_or(metadata, "authorization_endpoint", std::string{});
  parsed.token_endpoint = json_get_or(metadata, "token_endpoint", std::string{});
  if (parsed.issuer.empty()) return absl::InvalidArgumentError("authorization server metadata missing issuer");
  if (parsed.authorization_endpoint.empty()) {
    return absl::InvalidArgumentError("authorization server metadata missing authorization_endpoint");
  }
  if (parsed.token_endpoint.empty())
    return absl::InvalidArgumentError("authorization server metadata missing token_endpoint");
  auto scopes_supported = GetOptionalStringArray(metadata, "scopes_supported");
  if (!scopes_supported.ok()) return scopes_supported.status();
  parsed.scopes_supported = *scopes_supported;
  auto challenge_methods = GetOptionalStringArray(metadata, "code_challenge_methods_supported");
  if (!challenge_methods.ok()) return challenge_methods.status();
  parsed.code_challenge_methods_supported = *challenge_methods;
  if (const auto* cimd = json_at(metadata, "client_id_metadata_document_supported")) {
    if (!cimd->is_boolean()) {
      return absl::InvalidArgumentError("client_id_metadata_document_supported must be boolean");
    }
    parsed.client_id_metadata_document_supported = cimd->get<bool>();
  }
  return parsed;
}

absl::StatusOr<ClientIdMetadataDocument> ParseClientIdMetadataDocument(const nlohmann::json& metadata,
                                                                       absl::string_view document_url) {
  if (!metadata.is_object()) {
    return absl::InvalidArgumentError("Client ID Metadata Document must be an object");
  }
  if (!absl::StartsWith(document_url, "https://")) {
    return absl::InvalidArgumentError("Client ID Metadata Document URL must use https");
  }
  ClientIdMetadataDocument parsed;
  parsed.client_id = json_get_or(metadata, "client_id", std::string{});
  if (parsed.client_id != document_url) {
    return absl::PermissionDeniedError("Client ID Metadata Document client_id does not match its URL");
  }
  auto redirects = GetOptionalStringArray(metadata, "redirect_uris");
  if (!redirects.ok()) return redirects.status();
  if (redirects->empty()) {
    return absl::InvalidArgumentError("Client ID Metadata Document missing redirect_uris");
  }
  parsed.redirect_uris = std::move(*redirects);
  if (const auto* auth_method = json_at(metadata, "token_endpoint_auth_method")) {
    if (!auth_method->is_string()) {
      return absl::InvalidArgumentError("token_endpoint_auth_method must be a string");
    }
    parsed.token_endpoint_auth_method = auth_method->get<std::string>();
  }
  if (parsed.token_endpoint_auth_method != "none") {
    return absl::UnimplementedError("only public CIMD clients with token_endpoint_auth_method=none are supported");
  }
  return parsed;
}

absl::Status ValidateAuthorizationBinding(const ProtectedResourceMetadata& resource_metadata,
                                          absl::string_view expected_resource,
                                          const AuthorizationServerMetadata& server_metadata,
                                          absl::string_view selected_authorization_server) {
  if (resource_metadata.resource != expected_resource) {
    return absl::PermissionDeniedError("protected resource metadata resource mismatch");
  }
  bool selected_is_advertised = false;
  for (const std::string& candidate : resource_metadata.authorization_servers) {
    selected_is_advertised = selected_is_advertised || candidate == selected_authorization_server;
  }
  if (!selected_is_advertised) {
    return absl::PermissionDeniedError("authorization server was not advertised by the resource");
  }
  if (server_metadata.issuer != selected_authorization_server) {
    return absl::PermissionDeniedError("authorization server issuer mismatch");
  }
  bool supports_s256 = false;
  for (const std::string& method : server_metadata.code_challenge_methods_supported) {
    supports_s256 = supports_s256 || method == "S256";
  }
  if (!supports_s256) {
    return absl::FailedPreconditionError("authorization server does not advertise S256 PKCE");
  }
  return absl::OkStatus();
}

absl::StatusOr<std::vector<std::string>> MergeAuthorizationScopes(const std::vector<std::string>& previously_requested,
                                                                  const std::vector<std::string>& challenge_scopes,
                                                                  size_t max_scopes) {
  if (max_scopes == 0) {
    return absl::InvalidArgumentError("scope limit must be positive");
  }
  std::vector<std::string> merged;
  merged.reserve(std::min(max_scopes, previously_requested.size() + challenge_scopes.size()));
  absl::flat_hash_set<std::string> seen;
  auto append_unique = [&merged, &seen, max_scopes](const std::string& scope) -> absl::Status {
    if (scope.empty()) {
      return absl::InvalidArgumentError("scope values must not be empty");
    }
    if (seen.contains(scope)) return absl::OkStatus();
    if (merged.size() >= max_scopes) {
      return absl::ResourceExhaustedError("requested scope set exceeds limit");
    }
    seen.insert(scope);
    merged.push_back(scope);
    return absl::OkStatus();
  };
  for (const std::string& scope : previously_requested) {
    absl::Status status = append_unique(scope);
    if (!status.ok()) return status;
  }
  for (const std::string& scope : challenge_scopes) {
    absl::Status status = append_unique(scope);
    if (!status.ok()) return status;
  }
  return merged;
}

}  // namespace slop::mcp
