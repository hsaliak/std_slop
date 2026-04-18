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

  auto parsed_tokens_or = ParseTokenResponse(j, absl::ToUnixSeconds(absl::Now()));
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

absl::StatusOr<OAuthHandler::DeviceAuthorizationStart> OAuthHandler::StartOpenAiDeviceAuthorization() const {
  if (provider_ != Provider::kOpenAi) {
    return absl::FailedPreconditionError("Device authorization is only supported for OpenAI OAuth");
  }

  nlohmann::json request_body = {{"client_id", kOpenAiOAuthClientId}};
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
                                                             int64_t now_unix_seconds) const {
  OAuthTokens parsed_tokens;
  parsed_tokens.access_token = json_get_or(response, "access_token", std::string{});
  parsed_tokens.refresh_token = json_get_or(response, "refresh_token", std::string{});
  const int expires_in = json_get_or(response, "expires_in", 0);
  if (parsed_tokens.access_token.empty() || expires_in <= 0) {
    return absl::InternalError("OpenAI token response missing required fields");
  }
  parsed_tokens.expiry_time = now_unix_seconds + expires_in;
  if (provider_ == Provider::kOpenAi) {
    parsed_tokens.account_id = ExtractOpenAiAccountIdFromJwt(parsed_tokens.access_token);
  }
  return parsed_tokens;
}

absl::Status OAuthHandler::FetchOpenAiDeviceToken(const DeviceAuthorizationStart& start, std::ostream& out) {
  if (provider_ != Provider::kOpenAi) {
    return absl::FailedPreconditionError("Device authorization is only supported for OpenAI OAuth");
  }
  if (start.device_auth_id.empty()) {
    return absl::InvalidArgumentError("Device authorization ID is required");
  }

  const int64_t deadline = absl::ToUnixSeconds(absl::Now()) + start.expires_in_seconds;
  while (absl::ToUnixSeconds(absl::Now()) < deadline) {
    nlohmann::json poll_body = {{"client_id", kOpenAiOAuthClientId}, {"device_auth_id", start.device_auth_id}};
    auto response_or = http_client_->Post(kOpenAiOAuthDeviceTokenUrl, json_dump(poll_body), {"Content-Type: application/json"});
    if (response_or.ok()) {
      auto response_json = json_parse(*response_or);
      if (!response_json) {
        return absl::InternalError("Failed to parse OpenAI device token response");
      }
      auto parsed_tokens_or = ParseTokenResponse(*response_json, absl::ToUnixSeconds(absl::Now()));
      if (!parsed_tokens_or.ok()) {
        return parsed_tokens_or.status();
      }
      RETURN_IF_ERROR(SaveTokens(*parsed_tokens_or));
      out << "OpenAI OAuth token saved to " << token_path_ << "\n";
      return absl::OkStatus();
    }

    const std::string status_text(response_or.status().message());
    if (!absl::StrContains(status_text, "Terminal HTTP error: 400") ||
        (!absl::StrContains(status_text, "authorization_pending") && !absl::StrContains(status_text, "slow_down"))) {
      return absl::UnavailableError(absl::StrCat("OpenAI device authorization failed: ", status_text));
    }
    std::this_thread::sleep_for(std::chrono::seconds(start.interval_seconds));
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
