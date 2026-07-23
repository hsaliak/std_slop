#include "mcp/oauth_client.h"

#include <cstdlib>
#include <ctime>

#include "absl/status/status.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "core/json_utils.h"
#include "core/sha256.h"

namespace slop::mcp {
namespace {

std::string UrlEncode(const std::string& value) { return value; }

std::string RandomToken() {
  std::string raw = absl::StrCat(std::time(nullptr), ":", std::rand(), ":", std::rand());
  auto digest = Sha256Digest(raw);
  if (!digest.ok()) return raw;
  return absl::BytesToHexString(absl::string_view(reinterpret_cast<const char*>(digest->data()), digest->size()));
}

absl::StatusOr<OAuthTokenSet> ParseTokenResponse(const std::string& body) {
  auto parsed = json_parse(body);
  if (!parsed || !parsed->is_object()) return absl::InvalidArgumentError("OAuth token response is invalid JSON");
  OAuthTokenSet tokens;
  tokens.access_token = json_get_or(*parsed, "access_token", std::string{});
  tokens.refresh_token = json_get_or(*parsed, "refresh_token", std::string{});
  const int expires_in = json_get_or(*parsed, "expires_in", 0);
  tokens.expires_at_unix_seconds = expires_in <= 0 ? 0 : static_cast<int64_t>(std::time(nullptr)) + expires_in;
  if (tokens.access_token.empty()) return absl::InvalidArgumentError("OAuth token response missing access token");
  return tokens;
}

std::string FormBody(std::initializer_list<std::pair<std::string, std::string>> fields) {
  std::vector<std::string> parts;
  for (const auto& [key, value] : fields) parts.push_back(absl::StrCat(key, "=", UrlEncode(value)));
  return absl::StrJoin(parts, "&");
}

}  // namespace

absl::StatusOr<PkceAuthorizationSession> StartPkceAuthorization(const OAuthClientConfig& config) {
  if (config.client_id.empty()) return absl::InvalidArgumentError("OAuth client_id must not be empty");
  if (config.authorization_endpoint.empty()) return absl::InvalidArgumentError("OAuth authorization endpoint missing");
  if (config.token_endpoint.empty()) return absl::InvalidArgumentError("OAuth token endpoint missing");
  PkceAuthorizationSession session;
  session.state = RandomToken();
  session.code_verifier = RandomToken();
  session.redirect_uri = config.redirect_uri;
  auto challenge = Sha256Digest(session.code_verifier);
  if (!challenge.ok()) return challenge.status();
  const std::string code_challenge = absl::WebSafeBase64Escape(
      absl::string_view(reinterpret_cast<const char*>(challenge->data()), challenge->size()));
  session.authorization_url = absl::StrCat(config.authorization_endpoint, "?response_type=code&client_id=",
                                           UrlEncode(config.client_id), "&redirect_uri=", UrlEncode(session.redirect_uri),
                                           "&scope=", UrlEncode(absl::StrJoin(config.scopes, " ")), "&state=",
                                           UrlEncode(session.state), "&code_challenge=", code_challenge,
                                           "&code_challenge_method=S256");
  return session;
}

absl::StatusOr<std::string> ExtractAuthorizationCodeFromCallback(const std::string& callback_url,
                                                                  const std::string& expected_state) {
  const size_t query = callback_url.find('?');
  if (query == std::string::npos) return absl::InvalidArgumentError("OAuth callback missing query");
  std::string code;
  std::string state;
  for (const absl::string_view part : absl::StrSplit(callback_url.substr(query + 1), '&', absl::SkipEmpty())) {
    std::vector<std::string> kv = absl::StrSplit(part, '=');
    if (kv.size() != 2) continue;
    if (kv[0] == "code") code = kv[1];
    if (kv[0] == "state") state = kv[1];
  }
  if (state != expected_state) return absl::PermissionDeniedError("OAuth callback state mismatch");
  if (code.empty()) return absl::InvalidArgumentError("OAuth callback missing code");
  return code;
}

absl::StatusOr<OAuthTokenSet> ExchangeAuthorizationCode(HttpClient* http_client, const OAuthClientConfig& config,
                                                        const std::string& code,
                                                        const std::string& code_verifier) {
  if (http_client == nullptr) return absl::InvalidArgumentError("http_client must not be null");
  auto response = http_client->PostWithResponse(config.token_endpoint,
                                                FormBody({{"grant_type", "authorization_code"},
                                                          {"code", code},
                                                          {"client_id", config.client_id},
                                                          {"redirect_uri", config.redirect_uri},
                                                          {"code_verifier", code_verifier}}),
                                                {"Content-Type: application/x-www-form-urlencoded"});
  if (!response.ok()) return response.status();
  if (response->status_code < 200 || response->status_code >= 300) return absl::UnauthenticatedError("OAuth token exchange failed");
  return ParseTokenResponse(response->body);
}

absl::StatusOr<OAuthTokenSet> RefreshOAuthToken(HttpClient* http_client, const OAuthClientConfig& config,
                                                const std::string& refresh_token) {
  if (http_client == nullptr) return absl::InvalidArgumentError("http_client must not be null");
  if (refresh_token.empty()) return absl::InvalidArgumentError("OAuth refresh token must not be empty");
  auto response = http_client->PostWithResponse(config.token_endpoint,
                                                FormBody({{"grant_type", "refresh_token"},
                                                          {"refresh_token", refresh_token},
                                                          {"client_id", config.client_id}}),
                                                {"Content-Type: application/x-www-form-urlencoded"});
  if (!response.ok()) return response.status();
  if (response->status_code < 200 || response->status_code >= 300) return absl::UnauthenticatedError("OAuth refresh failed");
  return ParseTokenResponse(response->body);
}

}  // namespace slop::mcp
