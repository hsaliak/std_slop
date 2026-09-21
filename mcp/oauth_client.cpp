#include "mcp/oauth_client.h"

#include <array>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <limits>

#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "curl/curl.h"

#include "core/json_utils.h"
#include "core/sha256.h"

namespace slop::mcp {
namespace {

absl::StatusOr<std::string> UrlEncode(absl::string_view value) {
  if (value.empty()) return std::string{};
  if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return absl::ResourceExhaustedError("OAuth form value is too large");
  }
  char* escaped = curl_easy_escape(nullptr, value.data(), static_cast<int>(value.size()));
  if (escaped == nullptr) return absl::ResourceExhaustedError("Failed to encode OAuth form value");
  std::string encoded(escaped);
  curl_free(escaped);
  return encoded;
}

absl::StatusOr<std::string> UrlDecode(absl::string_view value) {
  if (value.empty()) return std::string{};
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '%') continue;
    if (i + 2 >= value.size() || !absl::ascii_isxdigit(value[i + 1]) ||
        !absl::ascii_isxdigit(value[i + 2])) {
      return absl::InvalidArgumentError("OAuth callback contains invalid percent encoding");
    }
    i += 2;
  }
  if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return absl::ResourceExhaustedError("OAuth callback value is too large");
  }
  // Form encoding treats literal '+' as space, but preserves escaped '%2B'.
  std::string component(value);
  for (char& c : component) {
    if (c == '+') c = ' ';
  }
  int decoded_size = 0;
  char* unescaped = curl_easy_unescape(nullptr, component.data(), static_cast<int>(component.size()), &decoded_size);
  if (unescaped == nullptr) return absl::ResourceExhaustedError("Failed to decode OAuth callback value");
  std::string decoded(unescaped, decoded_size);
  curl_free(unescaped);
  if (decoded.find('\0') != std::string::npos) {
    return absl::InvalidArgumentError("OAuth callback value contains NUL");
  }
  return decoded;
}

std::string RandomToken() {
  std::array<unsigned char, 32> bytes{};
  std::ifstream urandom("/dev/urandom", std::ios::binary);
  if (!urandom.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return std::string{};
  return absl::BytesToHexString(absl::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

bool IsHttpsUrl(const std::string& url) { return absl::StartsWith(url, "https://"); }

std::string OAuthErrorMessage(const nlohmann::json& parsed, const std::string& fallback) {
  const std::string error = json_get_or(parsed, "error", std::string{});
  if (error.empty()) return fallback;
  const std::string description = json_get_or(parsed, "error_description", std::string{});
  const std::string uri = json_get_or(parsed, "error_uri", std::string{});
  std::string message = absl::StrCat("OAuth token response error: ", error);
  if (!description.empty()) absl::StrAppend(&message, ": ", description);
  if (!uri.empty()) absl::StrAppend(&message, " (", uri, ")");
  return message;
}

absl::Status TokenHttpError(const HttpResponse& response, const std::string& fallback) {
  std::string message;
  auto parsed = json_parse(response.body);
  if (parsed.has_value() && parsed->is_object()) {
    message = OAuthErrorMessage(*parsed, fallback);
  } else {
    message = fallback;
  }
  return absl::UnauthenticatedError(absl::StrCat(message, " (HTTP ", response.status_code, ")"));
}

absl::StatusOr<OAuthTokenSet> ParseTokenResponse(const std::string& body) {
  auto parsed = json_parse(body);
  if (!parsed || !parsed->is_object()) return absl::InvalidArgumentError("OAuth token response is invalid JSON");
  const std::string oauth_error = json_get_or(*parsed, "error", std::string{});
  if (!oauth_error.empty()) return absl::UnauthenticatedError(OAuthErrorMessage(*parsed, "OAuth token response error"));
  OAuthTokenSet tokens;
  tokens.access_token = json_get_or(*parsed, "access_token", std::string{});
  tokens.refresh_token = json_get_or(*parsed, "refresh_token", std::string{});
  const auto token_type = json_get<std::string>(*parsed, "token_type");
  if (!token_type.has_value()) {
    return absl::InvalidArgumentError("OAuth token response missing token_type");
  }
  tokens.token_type = *token_type;
  tokens.scope = json_get_or(*parsed, "scope", std::string{});
  if (!absl::EqualsIgnoreCase(tokens.token_type, "Bearer")) {
    return absl::InvalidArgumentError("OAuth token response token_type must be Bearer");
  }
  tokens.token_type = "Bearer";
  const int expires_in = json_get_or(*parsed, "expires_in", 0);
  tokens.expires_at_unix_seconds = expires_in <= 0 ? 0 : static_cast<int64_t>(std::time(nullptr)) + expires_in;
  if (tokens.access_token.empty()) {
    return absl::InvalidArgumentError(
        "OAuth token response missing access_token and did not include an OAuth error field");
  }
  return tokens;
}

absl::StatusOr<std::string> FormBody(const std::vector<std::pair<std::string, std::string>>& fields) {
  std::vector<std::string> parts;
  for (const auto& [key, value] : fields) {
    auto encoded_key = UrlEncode(key);
    if (!encoded_key.ok()) return encoded_key.status();
    auto encoded_value = UrlEncode(value);
    if (!encoded_value.ok()) return encoded_value.status();
    parts.push_back(absl::StrCat(*encoded_key, "=", *encoded_value));
  }
  return absl::StrJoin(parts, "&");
}

}  // namespace

absl::StatusOr<PkceAuthorizationSession> StartPkceAuthorization(const OAuthClientConfig& config) {
  if (config.client_id.empty()) return absl::InvalidArgumentError("OAuth client_id must not be empty");
  if (absl::StartsWith(config.client_id, "https://") && !config.client_secret.empty()) {
    return absl::InvalidArgumentError("CIMD public clients must not configure a client_secret");
  }
  if (config.authorization_endpoint.empty()) return absl::InvalidArgumentError("OAuth authorization endpoint missing");
  if (config.token_endpoint.empty()) return absl::InvalidArgumentError("OAuth token endpoint missing");
  if (!IsHttpsUrl(config.authorization_endpoint) || !IsHttpsUrl(config.token_endpoint)) {
    return absl::InvalidArgumentError("OAuth endpoints must use https");
  }
  if (!config.s256_supported) {
    return absl::FailedPreconditionError("authorization server does not support S256 PKCE");
  }
  auto scopes = MergeAuthorizationScopes({}, config.scopes, config.max_scope_count);
  if (!scopes.ok()) return scopes.status();
  PkceAuthorizationSession session;
  session.state = RandomToken();
  session.code_verifier = RandomToken();
  if (session.state.empty() || session.code_verifier.empty()) {
    return absl::InternalError("Failed to generate OAuth random state");
  }
  session.redirect_uri = config.redirect_uri;
  auto challenge = Sha256Digest(session.code_verifier);
  if (!challenge.ok()) return challenge.status();
  const std::string code_challenge =
      absl::WebSafeBase64Escape(absl::string_view(reinterpret_cast<const char*>(challenge->data()), challenge->size()));
  std::vector<std::pair<std::string, std::string>> fields = {
      {"response_type", "code"},
      {"client_id", config.client_id},
      {"redirect_uri", session.redirect_uri},
  };
  if (!scopes->empty()) fields.push_back({"scope", absl::StrJoin(*scopes, " ")});
  if (!config.resource.empty()) fields.push_back({"resource", config.resource});
  fields.push_back({"state", session.state});
  fields.push_back({"code_challenge", code_challenge});
  fields.push_back({"code_challenge_method", "S256"});
  auto query = FormBody(fields);
  if (!query.ok()) return query.status();
  session.authorization_url = absl::StrCat(config.authorization_endpoint, "?", *query);
  return session;
}

absl::StatusOr<std::string> ExtractAuthorizationCodeFromCallback(const std::string& callback_url,
                                                                 const std::string& expected_state,
                                                                 const std::string& expected_issuer,
                                                                 const std::string& expected_redirect_uri,
                                                                 bool require_issuer) {
  const size_t query = callback_url.find('?');
  if (query == std::string::npos) return absl::InvalidArgumentError("OAuth callback missing query");
  if (callback_url.find('#') != std::string::npos) {
    return absl::InvalidArgumentError("OAuth callback must not contain a fragment");
  }
  if (!expected_redirect_uri.empty() && callback_url.substr(0, query) != expected_redirect_uri) {
    return absl::PermissionDeniedError("OAuth callback redirect URI mismatch");
  }
  std::string code;
  std::string state;
  std::string error;
  std::string error_description;
  std::string error_uri;
  std::string issuer;
  bool saw_code = false;
  bool saw_state = false;
  bool saw_issuer = false;
  for (const absl::string_view part : absl::StrSplit(callback_url.substr(query + 1), '&', absl::SkipEmpty())) {
    const size_t equals = part.find('=');
    if (equals == absl::string_view::npos) continue;
    auto key_or = UrlDecode(part.substr(0, equals));
    if (!key_or.ok()) return key_or.status();
    auto value_or = UrlDecode(part.substr(equals + 1));
    if (!value_or.ok()) return value_or.status();
    const std::string& key = *key_or;
    const std::string& value = *value_or;
    if (key == "code") {
      if (saw_code) return absl::InvalidArgumentError("OAuth callback contains duplicate code");
      saw_code = true;
      code = value;
    }
    if (key == "state") {
      if (saw_state) return absl::InvalidArgumentError("OAuth callback contains duplicate state");
      saw_state = true;
      state = value;
    }
    if (key == "error") error = value;
    if (key == "error_description") error_description = value;
    if (key == "error_uri") error_uri = value;
    if (key == "iss") {
      if (saw_issuer) return absl::InvalidArgumentError("OAuth callback contains duplicate issuer");
      saw_issuer = true;
      issuer = value;
    }
  }
  if (state != expected_state) return absl::PermissionDeniedError("OAuth callback state mismatch");
  if (require_issuer && !saw_issuer) {
    return absl::PermissionDeniedError("OAuth callback missing issuer");
  }
  if (saw_issuer && !expected_issuer.empty() && issuer != expected_issuer) {
    return absl::PermissionDeniedError("OAuth callback issuer mismatch");
  }
  if (!error.empty()) {
    std::string message = absl::StrCat("OAuth callback error: ", error);
    if (!error_description.empty()) absl::StrAppend(&message, ": ", error_description);
    if (!error_uri.empty()) absl::StrAppend(&message, " (", error_uri, ")");
    return absl::UnauthenticatedError(message);
  }
  if (code.empty()) return absl::InvalidArgumentError("OAuth callback missing code");
  return code;
}

absl::StatusOr<OAuthTokenSet> ExchangeAuthorizationCode(HttpClient* http_client, const OAuthClientConfig& config,
                                                        const std::string& code, const std::string& code_verifier) {
  if (http_client == nullptr) return absl::InvalidArgumentError("http_client must not be null");
  if (!IsHttpsUrl(config.token_endpoint)) return absl::InvalidArgumentError("OAuth token endpoint must use https");
  if (absl::StartsWith(config.client_id, "https://") && !config.client_secret.empty()) {
    return absl::InvalidArgumentError("CIMD public clients must not configure a client_secret");
  }
  std::vector<std::pair<std::string, std::string>> fields = {{"grant_type", "authorization_code"},
                                                             {"code", code},
                                                             {"client_id", config.client_id},
                                                             {"redirect_uri", config.redirect_uri},
                                                             {"code_verifier", code_verifier}};
  if (!config.resource.empty()) fields.push_back({"resource", config.resource});
  if (!config.client_secret.empty()) fields.push_back({"client_secret", config.client_secret});
  auto body = FormBody(fields);
  if (!body.ok()) return body.status();
  auto response = http_client->PostOnceWithResponse(
      config.token_endpoint, *body,
      {"Accept: application/json", "Content-Type: application/x-www-form-urlencoded"});
  if (!response.ok()) return response.status();
  if (response->status_code < 200 || response->status_code >= 300) {
    return TokenHttpError(*response, "OAuth token exchange failed");
  }
  auto tokens = ParseTokenResponse(response->body);
  if (!tokens.ok()) return tokens.status();
  tokens->issuer = config.issuer;
  tokens->resource = config.resource;
  return *tokens;
}

absl::StatusOr<OAuthTokenSet> RefreshOAuthToken(HttpClient* http_client, const OAuthClientConfig& config,
                                                const std::string& refresh_token) {
  if (http_client == nullptr) return absl::InvalidArgumentError("http_client must not be null");
  if (!IsHttpsUrl(config.token_endpoint)) return absl::InvalidArgumentError("OAuth token endpoint must use https");
  if (absl::StartsWith(config.client_id, "https://") && !config.client_secret.empty()) {
    return absl::InvalidArgumentError("CIMD public clients must not configure a client_secret");
  }
  if (refresh_token.empty()) return absl::InvalidArgumentError("OAuth refresh token must not be empty");
  std::vector<std::pair<std::string, std::string>> fields = {
      {"grant_type", "refresh_token"}, {"refresh_token", refresh_token}, {"client_id", config.client_id}};
  if (!config.resource.empty()) fields.push_back({"resource", config.resource});
  if (!config.client_secret.empty()) fields.push_back({"client_secret", config.client_secret});
  auto body = FormBody(fields);
  if (!body.ok()) return body.status();
  auto response = http_client->PostOnceWithResponse(
      config.token_endpoint, *body,
      {"Accept: application/json", "Content-Type: application/x-www-form-urlencoded"});
  if (!response.ok()) return response.status();
  if (response->status_code < 200 || response->status_code >= 300) {
    return TokenHttpError(*response, "OAuth refresh failed");
  }
  auto tokens = ParseTokenResponse(response->body);
  if (!tokens.ok()) return tokens.status();
  tokens->issuer = config.issuer;
  tokens->resource = config.resource;
  if (tokens->refresh_token.empty()) tokens->refresh_token = refresh_token;
  return *tokens;
}

}  // namespace slop::mcp
