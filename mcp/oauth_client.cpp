#include "mcp/oauth_client.h"

#include <array>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>

#include "absl/status/status.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "core/json_utils.h"
#include "core/sha256.h"

namespace slop::mcp {
namespace {

std::string UrlEncode(const std::string& value) {
  std::string encoded;
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (const unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded.push_back(static_cast<char>(c));
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[c >> 4]);
      encoded.push_back(kHex[c & 0x0F]);
    }
  }
  return encoded;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string UrlDecode(absl::string_view value) {
  std::string decoded;
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      const int hi = HexValue(value[i + 1]);
      const int lo = HexValue(value[i + 2]);
      if (hi >= 0 && lo >= 0) {
        decoded.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    decoded.push_back(value[i] == '+' ? ' ' : value[i]);
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
  const int expires_in = json_get_or(*parsed, "expires_in", 0);
  tokens.expires_at_unix_seconds = expires_in <= 0 ? 0 : static_cast<int64_t>(std::time(nullptr)) + expires_in;
  if (tokens.access_token.empty()) {
    return absl::InvalidArgumentError(
        "OAuth token response missing access_token and did not include an OAuth error field");
  }
  return tokens;
}

std::string FormBody(const std::vector<std::pair<std::string, std::string>>& fields) {
  std::vector<std::string> parts;
  for (const auto& [key, value] : fields) parts.push_back(absl::StrCat(key, "=", UrlEncode(value)));
  return absl::StrJoin(parts, "&");
}

}  // namespace

absl::StatusOr<PkceAuthorizationSession> StartPkceAuthorization(const OAuthClientConfig& config) {
  if (config.client_id.empty()) return absl::InvalidArgumentError("OAuth client_id must not be empty");
  if (config.authorization_endpoint.empty()) return absl::InvalidArgumentError("OAuth authorization endpoint missing");
  if (config.token_endpoint.empty()) return absl::InvalidArgumentError("OAuth token endpoint missing");
  if (!IsHttpsUrl(config.authorization_endpoint) || !IsHttpsUrl(config.token_endpoint)) {
    return absl::InvalidArgumentError("OAuth endpoints must use https");
  }
  PkceAuthorizationSession session;
  session.state = RandomToken();
  session.code_verifier = RandomToken();
  if (session.state.empty() || session.code_verifier.empty()) {
    return absl::InternalError("Failed to generate OAuth random state");
  }
  session.redirect_uri = config.redirect_uri;
  auto challenge = Sha256Digest(session.code_verifier);
  if (!challenge.ok()) return challenge.status();
  const std::string code_challenge = absl::WebSafeBase64Escape(
      absl::string_view(reinterpret_cast<const char*>(challenge->data()), challenge->size()));
  session.authorization_url = absl::StrCat(config.authorization_endpoint, "?response_type=code&client_id=",
                                           UrlEncode(config.client_id), "&redirect_uri=", UrlEncode(session.redirect_uri));
  if (!config.scopes.empty()) {
    absl::StrAppend(&session.authorization_url, "&scope=", UrlEncode(absl::StrJoin(config.scopes, " ")));
  }
  absl::StrAppend(&session.authorization_url, "&state=", UrlEncode(session.state), "&code_challenge=", code_challenge,
                  "&code_challenge_method=S256");
  return session;
}

absl::StatusOr<std::string> ExtractAuthorizationCodeFromCallback(const std::string& callback_url,
                                                                  const std::string& expected_state) {
  const size_t query = callback_url.find('?');
  if (query == std::string::npos) return absl::InvalidArgumentError("OAuth callback missing query");
  std::string code;
  std::string state;
  std::string error;
  std::string error_description;
  std::string error_uri;
  for (const absl::string_view part : absl::StrSplit(callback_url.substr(query + 1), '&', absl::SkipEmpty())) {
    const size_t equals = part.find('=');
    if (equals == absl::string_view::npos) continue;
    const std::string key(part.substr(0, equals));
    const std::string value = UrlDecode(part.substr(equals + 1));
    if (key == "code") code = value;
    if (key == "state") state = value;
    if (key == "error") error = value;
    if (key == "error_description") error_description = value;
    if (key == "error_uri") error_uri = value;
  }
  if (state != expected_state) return absl::PermissionDeniedError("OAuth callback state mismatch");
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
                                                        const std::string& code,
                                                        const std::string& code_verifier) {
  if (http_client == nullptr) return absl::InvalidArgumentError("http_client must not be null");
  if (!IsHttpsUrl(config.token_endpoint)) return absl::InvalidArgumentError("OAuth token endpoint must use https");
  std::vector<std::pair<std::string, std::string>> fields = {{"grant_type", "authorization_code"},
                                                              {"code", code},
                                                              {"client_id", config.client_id},
                                                              {"redirect_uri", config.redirect_uri},
                                                              {"code_verifier", code_verifier}};
  if (!config.client_secret.empty()) fields.push_back({"client_secret", config.client_secret});
  auto response = http_client->PostWithResponse(
      config.token_endpoint, FormBody(fields), {"Accept: application/json", "Content-Type: application/x-www-form-urlencoded"});
  if (!response.ok()) return response.status();
  if (response->status_code < 200 || response->status_code >= 300) {
    return TokenHttpError(*response, "OAuth token exchange failed");
  }
  return ParseTokenResponse(response->body);
}

absl::StatusOr<OAuthTokenSet> RefreshOAuthToken(HttpClient* http_client, const OAuthClientConfig& config,
                                                const std::string& refresh_token) {
  if (http_client == nullptr) return absl::InvalidArgumentError("http_client must not be null");
  if (!IsHttpsUrl(config.token_endpoint)) return absl::InvalidArgumentError("OAuth token endpoint must use https");
  if (refresh_token.empty()) return absl::InvalidArgumentError("OAuth refresh token must not be empty");
  std::vector<std::pair<std::string, std::string>> fields = {{"grant_type", "refresh_token"},
                                                              {"refresh_token", refresh_token},
                                                              {"client_id", config.client_id}};
  if (!config.client_secret.empty()) fields.push_back({"client_secret", config.client_secret});
  auto response = http_client->PostWithResponse(
      config.token_endpoint, FormBody(fields), {"Accept: application/json", "Content-Type: application/x-www-form-urlencoded"});
  if (!response.ok()) return response.status();
  if (response->status_code < 200 || response->status_code >= 300) {
    return TokenHttpError(*response, "OAuth refresh failed");
  }
  return ParseTokenResponse(response->body);
}

}  // namespace slop::mcp
