#include "core/oauth_handler.h"

#include <unistd.h>

#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>
#include <sstream>
#include <system_error>

#include "absl/log/log.h"
#include "absl/numeric/bits.h"
#include "absl/random/random.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/time/clock.h"
#include "nlohmann/json.hpp"

#include "core/constants.h"
#include "core/shell_util.h"
#include "core/status_macros.h"
#include "json_utils.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace slop {

namespace {

constexpr std::array<uint32_t, 64> kSha256RoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

uint32_t LoadBigEndian32(const unsigned char* ptr) {
  return (static_cast<uint32_t>(ptr[0]) << 24) | (static_cast<uint32_t>(ptr[1]) << 16) |
         (static_cast<uint32_t>(ptr[2]) << 8) | static_cast<uint32_t>(ptr[3]);
}

std::array<unsigned char, 32> Sha256(absl::string_view input) {
  std::array<uint32_t, 8> state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::string padded(input);
  const uint64_t bit_length = static_cast<uint64_t>(input.size()) * 8;
  padded.push_back(static_cast<char>(0x80));
  while ((padded.size() % 64) != 56) {
    padded.push_back('\0');
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    padded.push_back(static_cast<char>((bit_length >> shift) & 0xff));
  }

  std::array<uint32_t, 64> schedule;
  for (size_t offset = 0; offset < padded.size(); offset += 64) {
    const unsigned char* chunk = reinterpret_cast<const unsigned char*>(padded.data() + offset);
    for (int i = 0; i < 16; ++i) {
      schedule[i] = LoadBigEndian32(chunk + (i * 4));
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = absl::rotr(schedule[i - 15], 7) ^ absl::rotr(schedule[i - 15], 18) ^
                          (schedule[i - 15] >> 3);
      const uint32_t s1 = absl::rotr(schedule[i - 2], 17) ^ absl::rotr(schedule[i - 2], 19) ^
                          (schedule[i - 2] >> 10);
      schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t s1 = absl::rotr(e, 6) ^ absl::rotr(e, 11) ^ absl::rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t temp1 = h + s1 + ch + kSha256RoundConstants[i] + schedule[i];
      const uint32_t s0 = absl::rotr(a, 2) ^ absl::rotr(a, 13) ^ absl::rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t temp2 = s0 + maj;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  }

  std::array<unsigned char, 32> digest;
  for (int i = 0; i < 8; ++i) {
    digest[i * 4] = static_cast<unsigned char>((state[i] >> 24) & 0xff);
    digest[i * 4 + 1] = static_cast<unsigned char>((state[i] >> 16) & 0xff);
    digest[i * 4 + 2] = static_cast<unsigned char>((state[i] >> 8) & 0xff);
    digest[i * 4 + 3] = static_cast<unsigned char>(state[i] & 0xff);
  }
  return digest;
}

std::string Base64UrlEncode(absl::string_view input) {
  std::string encoded;
  absl::Base64Escape(input, &encoded);
  for (char& c : encoded) {
    if (c == '+') c = '-';
    if (c == '/') c = '_';
  }
  while (!encoded.empty() && encoded.back() == '=') {
    encoded.pop_back();
  }
  return encoded;
}

std::string UrlEncodeFormValue(const std::string& value) {
  std::ostringstream encoded;
  encoded << std::uppercase << std::hex;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded << static_cast<char>(c);
    } else {
      encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
  }
  return encoded.str();
}

constexpr char kFetchOauthGuidance[] = "Run: std_slop --fetch-oauth";

std::string UrlDecode(std::string_view value) {
  std::string decoded;
  decoded.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (c == '+') {
      decoded.push_back(' ');
      continue;
    }
    if (c == '%' && i + 2 < value.size() && std::isxdigit(value[i + 1]) && std::isxdigit(value[i + 2])) {
      const std::string hex(value.substr(i + 1, 2));
      decoded.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
      i += 2;
      continue;
    }
    decoded.push_back(c);
  }
  return decoded;
}

std::string ExtractUrlParam(std::string_view url, std::string_view name) {
  const std::string token = absl::StrCat(name, "=");
  size_t pos = url.find(token);
  while (pos != std::string_view::npos) {
    if (pos == 0 || url[pos - 1] == '?' || url[pos - 1] == '#' || url[pos - 1] == '&') {
      const size_t value_start = pos + token.size();
      const size_t value_end = url.find_first_of("&#", value_start);
      return UrlDecode(url.substr(value_start, value_end == std::string_view::npos ? std::string_view::npos
                                                                                   : value_end - value_start));
    }
    pos = url.find(token, pos + 1);
  }
  return "";
}

std::string BuildOpenAiAuthorizeUrl(const OAuthHandler::ManualAuthorizationSession& session,
                                    const std::string& code_challenge) {
  return absl::StrCat(kOpenAiOAuthAuthorizeUrl, "?response_type=code", "&client_id=",
                      UrlEncodeFormValue(kOpenAiOAuthClientId), "&redirect_uri=",
                      UrlEncodeFormValue(session.redirect_uri), "&scope=", UrlEncodeFormValue(kOpenAiOAuthScope),
                      "&code_challenge=", UrlEncodeFormValue(code_challenge), "&code_challenge_method=S256",
                      "&id_token_add_organizations=true&codex_cli_simplified_flow=true&state=",
                      UrlEncodeFormValue(session.state), "&originator=", UrlEncodeFormValue(kOpenAiOAuthOriginator));
}

absl::Status OAuthFailureStatus(absl::StatusCode code, absl::string_view message) {
  return absl::Status(code, absl::StrCat(message, ". ", kFetchOauthGuidance));
}
}  // namespace
absl::Status MaybeCreateDirectory(const std::string& dir_path) {
  std::error_code ec;
  if (!std::filesystem::create_directories(dir_path, ec) && ec) {
    return absl::InternalError(ec.message());
  }

  return absl::OkStatus();
}

OAuthHandler::OAuthHandler(HttpClient* http_client) : OAuthHandler(http_client, Provider::kOpenAi) {}

OAuthHandler::OAuthHandler(HttpClient* http_client, Provider provider)
    : http_client_(http_client), provider_(provider) {
  std::string home = GetHomeDir();
  if (!home.empty()) {
    token_path_ = home + "/.config/slop/chatgpt_plus_token.json";
  }
}

absl::Status OAuthHandler::LoadTokens() {
  if (token_path_.empty()) return absl::NotFoundError("No home directory found");
  LOG(INFO) << "Loading tokens from " << token_path_;
  std::ifstream f(token_path_);
  if (!f.is_open()) {
    LOG(WARNING) << "Token file not found at " << token_path_;
    return OAuthFailureStatus(absl::StatusCode::kNotFound,
                              absl::StrCat("Token file not found at ", token_path_));
  }

  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto j_opt = json_parse(content);
  if (!j_opt) return absl::InternalError("Failed to parse tokens");
  const auto& j = *j_opt;

  tokens_.access_token = json_get_or(j, "access_token", std::string{});
  tokens_.refresh_token = json_get_or(j, "refresh_token", std::string{});
  tokens_.expiry_time = json_get_or(j, "expiry_time", 0LL);
  tokens_.account_id = json_get_or(j, "account_id", std::string{});
  if (provider_ == Provider::kOpenAi && tokens_.account_id.empty()) {
    tokens_.account_id = ExtractOpenAiAccountIdFromJwt(tokens_.access_token);
  }
  return absl::OkStatus();
}

absl::Status OAuthHandler::SaveTokens(const OAuthTokens& tokens) {
  if (token_path_.empty()) return absl::InternalError("No token path");

  std::string dir = token_path_.substr(0, token_path_.find_last_of('/'));
  auto mkdir_status = MaybeCreateDirectory(dir);
  if (!mkdir_status.ok()) return mkdir_status;

  nlohmann::json j;
  j["access_token"] = tokens.access_token;
  j["refresh_token"] = tokens.refresh_token;
  j["expiry_time"] = tokens.expiry_time;
  j["account_id"] = tokens.account_id;

  std::ofstream f(token_path_);
  if (!f.is_open()) return absl::InternalError("Failed to open token file for writing");
  f << j.dump(4, ' ', false, nlohmann::json::error_handler_t::replace);
  tokens_ = tokens;
  return absl::OkStatus();
}

absl::StatusOr<std::string> OAuthHandler::GetValidToken() {
  if (!enabled_) return absl::FailedPreconditionError("OAuth not enabled");
  if (tokens_.access_token.empty()) {
    auto status = LoadTokens();
    if (!status.ok()) return status;
  }

  if (absl::ToUnixSeconds(absl::Now()) >= tokens_.expiry_time - 60) {
    auto status = RefreshToken();
    if (!status.ok()) return status;
  }

  return tokens_.access_token;
}

absl::Status OAuthHandler::RefreshToken() {
  if (tokens_.refresh_token.empty()) {
    LOG(ERROR) << "No refresh token available";
    return OAuthFailureStatus(absl::StatusCode::kUnauthenticated, "No refresh token available");
  }

  LOG(INFO) << "Refreshing OAuth token...";
  std::string token_url;
  std::string body;
  {
    token_url = kOpenAiOAuthTokenUrl;
    const char* client_secret = std::getenv("CHATGPT_CLIENT_SECRET");
    body = absl::StrCat("refresh_token=", UrlEncodeFormValue(tokens_.refresh_token),
                        "&client_id=", UrlEncodeFormValue(kOpenAiOAuthClientId), "&grant_type=refresh_token");
    if (client_secret != nullptr && *client_secret != '\0') {
      absl::StrAppend(&body, "&client_secret=", UrlEncodeFormValue(client_secret));
    }
  }

  auto res = http_client_->Post(token_url, body, {"Content-Type: application/x-www-form-urlencoded"});
  if (!res.ok()) {
    LOG(ERROR) << "Token refresh failed: " << res.status().ToString();
    return OAuthFailureStatus(absl::StatusCode::kUnauthenticated, "Token refresh failed");
  }
  LOG(INFO) << "Token refreshed successfully.";

  auto j_opt = json_parse(*res);
  if (!j_opt) return absl::InternalError("Failed to parse refresh response");
  const auto& j = *j_opt;

  auto parsed_tokens_or = ParseTokenResponse(j, absl::ToUnixSeconds(absl::Now()), /*require_refresh_token=*/false);
  if (!parsed_tokens_or.ok()) {
    return parsed_tokens_or.status();
  }
  parsed_tokens_or->refresh_token = tokens_.refresh_token;
  return SaveTokens(*parsed_tokens_or);
}

absl::StatusOr<std::string> OAuthHandler::GetOpenAiAccountId() {
  if (provider_ != Provider::kOpenAi) {
    return absl::FailedPreconditionError("Account ID is only available for OpenAI OAuth");
  }
  auto token_or = GetValidToken();
  if (!token_or.ok()) {
    return token_or.status();
  }
  if (tokens_.account_id.empty()) {
    tokens_.account_id = ExtractOpenAiAccountIdFromJwt(*token_or);
    if (!tokens_.account_id.empty()) {
      (void)SaveTokens(tokens_);
    }
  }
  if (tokens_.account_id.empty()) {
    return absl::NotFoundError("OpenAI account ID not found in token");
  }
  return tokens_.account_id;
}

std::string OAuthHandler::GenerateOAuthRandomValue() const {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
  absl::BitGen bitgen;
  std::string value;
  value.reserve(64);
  for (int i = 0; i < 64; ++i) {
    value.push_back(kAlphabet[absl::Uniform<int>(bitgen, 0, static_cast<int>(sizeof(kAlphabet) - 1))]);
  }
  return value;
}

absl::StatusOr<std::string> OAuthHandler::BuildPkceCodeChallenge(const std::string& code_verifier) const {
  if (code_verifier.empty()) {
    return absl::InvalidArgumentError("PKCE code verifier is required");
  }
  const std::array<unsigned char, 32> digest = Sha256(code_verifier);
  const std::string digest_bytes(reinterpret_cast<const char*>(digest.data()), digest.size());
  return Base64UrlEncode(digest_bytes);
}

absl::StatusOr<OAuthHandler::ManualAuthorizationSession> OAuthHandler::StartOpenAiManualAuthorization() const {
  if (provider_ != Provider::kOpenAi) {
    return absl::FailedPreconditionError("Manual authorization is only supported for OpenAI OAuth");
  }
  ManualAuthorizationSession session;
  session.state = GenerateOAuthRandomValue();
  session.code_verifier = GenerateOAuthRandomValue();
  session.redirect_uri = kOpenAiOAuthRedirectUrl;
  auto code_challenge_or = BuildPkceCodeChallenge(session.code_verifier);
  if (!code_challenge_or.ok()) {
    return code_challenge_or.status();
  }
  session.authorization_uri = BuildOpenAiAuthorizeUrl(session, *code_challenge_or);
  return session;
}

absl::Status OAuthHandler::CompleteOpenAiManualAuthorization(const ManualAuthorizationSession& session,
                                                             const std::string& callback_url,
                                                             std::ostream& out) {
  const std::string state_from_url = ExtractUrlParam(callback_url, "state");
  if (state_from_url.empty()) {
    return absl::InvalidArgumentError("OpenAI callback URL did not contain a state parameter");
  }
  if (state_from_url != session.state) {
    return absl::InvalidArgumentError("OpenAI callback state mismatch");
  }
  const std::string error = ExtractUrlParam(callback_url, "error");
  const std::string error_description = ExtractUrlParam(callback_url, "error_description");
  if (!error.empty() || !error_description.empty()) {
    return absl::InvalidArgumentError(
        absl::StrCat("OpenAI authorization failed", error.empty() ? "" : absl::StrCat(": ", error),
                     error_description.empty() ? "" : absl::StrCat(" (", error_description, ")")));
  }
  const std::string authorization_code = ExtractUrlParam(callback_url, "code");
  if (authorization_code.empty()) {
    return absl::InvalidArgumentError("OpenAI callback URL did not contain a code parameter");
  }
  return ExchangeOpenAiAuthorizationCodeForTokens(authorization_code, session.code_verifier, session.redirect_uri, out);
}

absl::Status OAuthHandler::ExchangeOpenAiAuthorizationCodeForTokens(const std::string& authorization_code,
                                                                    const std::string& code_verifier,
                                                                    const std::string& redirect_uri,
                                                                    std::ostream& out) {
  if (authorization_code.empty()) {
    return absl::InvalidArgumentError("OpenAI authorization code is required");
  }
  if (code_verifier.empty()) {
    return absl::InvalidArgumentError("OpenAI code verifier is required");
  }
  std::string body = absl::StrCat("grant_type=authorization_code&code=", UrlEncodeFormValue(authorization_code),
                                  "&redirect_uri=", UrlEncodeFormValue(redirect_uri), "&client_id=",
                                  UrlEncodeFormValue(kOpenAiOAuthClientId), "&code_verifier=",
                                  UrlEncodeFormValue(code_verifier));
  const char* client_secret = std::getenv("CHATGPT_CLIENT_SECRET");
  if (client_secret != nullptr && *client_secret != '\0') {
    body = absl::StrCat(body, "&client_secret=", UrlEncodeFormValue(client_secret));
  }
  auto response_or = http_client_->Post(kOpenAiOAuthTokenUrl, body,
                                        {"Content-Type: application/x-www-form-urlencoded"});
  if (!response_or.ok()) {
    return absl::UnavailableError(absl::StrCat("OpenAI token exchange failed: ", response_or.status().message()));
  }
  auto response_json = json_parse(*response_or);
  if (!response_json) {
    return absl::InternalError("Failed to parse OpenAI token exchange response");
  }
  auto parsed_tokens_or = ParseTokenResponse(*response_json, absl::ToUnixSeconds(absl::Now()),
                                             /*require_refresh_token=*/true);
  if (!parsed_tokens_or.ok()) {
    return parsed_tokens_or.status();
  }
  RETURN_IF_ERROR(SaveTokens(*parsed_tokens_or));
  out << "OpenAI OAuth token saved to " << token_path_ << "\n";
  return absl::OkStatus();
}

absl::StatusOr<OAuthHandler::DeviceAuthorizationStart> OAuthHandler::StartOpenAiDeviceAuthorization() const {
  if (provider_ != Provider::kOpenAi) {
    return absl::FailedPreconditionError("Device authorization is only supported for OpenAI OAuth");
  }

  nlohmann::json request_body = {{"client_id", kOpenAiOAuthClientId},
                                 {"scope", kOpenAiOAuthScope},
                                 {"redirect_uri", kOpenAiOAuthDeviceRedirectUrl}};
  auto response_or = http_client_->Post(kOpenAiOAuthDeviceUserCodeUrl, json_dump(request_body),
                                        {"Content-Type: application/json"});
  if (!response_or.ok()) {
    return absl::UnavailableError("Failed to initiate OpenAI device authorization");
  }
  auto response_json = json_parse(*response_or);
  if (!response_json) {
    return absl::InternalError("Failed to parse OpenAI device authorization response");
  }

  DeviceAuthorizationStart start;
  start.device_auth_id = json_get_or(*response_json, "device_auth_id", std::string{});
  start.user_code = json_get_or(*response_json, "user_code", std::string{});
  start.verification_uri = json_get_or(*response_json, "verification_uri", std::string{kOpenAiOAuthDeviceVerificationUrl});
  start.interval_seconds = std::max(1, json_get_or(*response_json, "interval", 5));
  start.expires_in_seconds = std::max(1, json_get_or(*response_json, "expires_in", 900));
  if (start.device_auth_id.empty() || start.user_code.empty()) {
    return absl::InternalError("OpenAI device authorization response missing required fields");
  }
  return start;
}

absl::StatusOr<OAuthTokens> OAuthHandler::ParseTokenResponse(const nlohmann::json& response,
                                                             int64_t now_unix_seconds,
                                                             bool require_refresh_token) const {
  OAuthTokens parsed_tokens;
  parsed_tokens.access_token = json_get_or(response, "access_token", std::string{});
  parsed_tokens.refresh_token = json_get_or(response, "refresh_token", std::string{});
  const int expires_in = json_get_or(response, "expires_in", 0);
  if (parsed_tokens.access_token.empty() || expires_in <= 0) {
    return absl::InternalError("OpenAI token response missing required fields");
  }
  if (require_refresh_token && parsed_tokens.refresh_token.empty()) {
    return absl::InternalError("OpenAI token response missing required refresh_token");
  }
  parsed_tokens.expiry_time = now_unix_seconds + expires_in;
  if (provider_ == Provider::kOpenAi) {
    parsed_tokens.account_id = ExtractOpenAiAccountIdFromJwt(parsed_tokens.access_token);
  }
  return parsed_tokens;
}

void OAuthHandler::SleepForDevicePoll(std::chrono::seconds delay) const {
  std::this_thread::sleep_for(delay);
}

absl::Status OAuthHandler::FetchOpenAiDeviceToken(const DeviceAuthorizationStart& start, std::ostream& out) {
  if (provider_ != Provider::kOpenAi) {
    return absl::FailedPreconditionError("Device authorization is only supported for OpenAI OAuth");
  }
  if (start.device_auth_id.empty()) {
    return absl::InvalidArgumentError("Device authorization ID is required");
  }

  const int64_t deadline = absl::ToUnixSeconds(absl::Now()) + start.expires_in_seconds;
  int current_interval_seconds = std::max(1, start.interval_seconds);
  while (absl::ToUnixSeconds(absl::Now()) < deadline) {
    nlohmann::json poll_body = {{"client_id", kOpenAiOAuthClientId},
                                {"device_auth_id", start.device_auth_id},
                                {"user_code", start.user_code}};
    auto response_or = http_client_->Post(kOpenAiOAuthDeviceTokenUrl, json_dump(poll_body), {"Content-Type: application/json"});
    if (response_or.ok()) {
      auto response_json = json_parse(*response_or);
      if (!response_json) {
        return absl::InternalError("Failed to parse OpenAI device token response");
      }
      const std::string authorization_code = json_get_or(*response_json, "authorization_code", std::string{});
      const std::string code_verifier = json_get_or(*response_json, "code_verifier", std::string{});
      if (authorization_code.empty() || code_verifier.empty()) {
        return absl::InternalError("OpenAI device poll response missing authorization_code/code_verifier");
      }
      return ExchangeOpenAiAuthorizationCodeForTokens(authorization_code, code_verifier,
                                                      kOpenAiOAuthDeviceRedirectUrl, out);
    }

    const std::string status_text(response_or.status().message());
    if (!absl::StrContains(status_text, "Terminal HTTP error: 400") ||
        (!absl::StrContains(status_text, "authorization_pending") && !absl::StrContains(status_text, "slow_down"))) {
      return absl::UnavailableError(absl::StrCat("OpenAI device authorization failed: ", status_text));
    }
    if (absl::StrContains(status_text, "slow_down")) {
      ++current_interval_seconds;
    }
    SleepForDevicePoll(std::chrono::seconds(current_interval_seconds));
  }
  return absl::DeadlineExceededError("OpenAI device authorization timed out");
}

std::string OAuthHandler::ExtractOpenAiAccountIdFromJwt(const std::string& jwt) {
  if (jwt.empty()) {
    return "";
  }
  const std::vector<std::string> parts = absl::StrSplit(jwt, '.');
  if (parts.size() != 3 || parts[1].empty()) {
    return "";
  }

  std::string payload_json;
  if (!absl::WebSafeBase64Unescape(parts[1], &payload_json)) {
    return "";
  }
  auto payload_opt = json_parse(payload_json);
  if (!payload_opt) {
    return "";
  }

  const auto& payload = *payload_opt;
  const auto* auth_claims = json_at(payload, "https://api.openai.com/auth");
  if (auth_claims != nullptr) {
    const std::string account_id = json_get_or(*auth_claims, "chatgpt_account_id", std::string{});
    if (!account_id.empty()) {
      return account_id;
    }
  }

  std::string direct_account_id = json_get_or(payload, "chatgpt_account_id", std::string{});
  if (!direct_account_id.empty()) {
    return direct_account_id;
  }

  const auto organizations = json_get<nlohmann::json::array_t>(payload, "organizations");
  if (organizations && !organizations->empty()) {
    return json_get_or((*organizations)[0], "id", std::string{});
  }
  return "";
}

}  // namespace slop
