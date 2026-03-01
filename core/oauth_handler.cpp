#include "core/oauth_handler.h"

#include <unistd.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>
#include <cstdlib>

#include "absl/log/log.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/time/clock.h"
#include "nlohmann/json.hpp"

#include "core/constants.h"
#include "core/shell_util.h"
#include "json_utils.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
namespace slop {

namespace {
const char* kGeminiClientId = "681255809395-oo8ft2oprdrnp9e3aqf6av3hmdib135j.apps.googleusercontent.com";
const char* kGeminiClientSecret = "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";
}  // namespace
absl::Status MaybeCreateDirectory(const std::string& dir_path) {
  std::error_code ec;
  if (!std::filesystem::create_directories(dir_path, ec) && ec) {
    return absl::InternalError(ec.message());
  }

  return absl::OkStatus();
}

OAuthHandler::OAuthHandler(HttpClient* http_client) : OAuthHandler(http_client, Provider::kGoogle) {}

OAuthHandler::OAuthHandler(HttpClient* http_client, Provider provider) : http_client_(http_client), provider_(provider) {
  std::string home = GetHomeDir();
  if (!home.empty()) {
    token_path_ = provider_ == Provider::kGoogle ? home + "/.config/slop/token.json"
                                                  : home + "/.config/slop/chatgpt_plus_token.json";
  }
}

absl::Status OAuthHandler::LoadTokens() {
  if (token_path_.empty()) return absl::NotFoundError("No home directory found");
  LOG(INFO) << "Loading tokens from " << token_path_;
  std::ifstream f(token_path_);
  if (!f.is_open()) {
    LOG(WARNING) << "Token file not found at " << token_path_;
    return absl::NotFoundError("Token file not found. Please run ./slop_auth.sh");
  }

  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto j_opt = json_parse(content);
  if (!j_opt) return absl::InternalError("Failed to parse tokens");
  const auto& j = *j_opt;

  tokens_.access_token = json_get_or(j, "access_token", std::string{});
  tokens_.refresh_token = json_get_or(j, "refresh_token", std::string{});
  tokens_.expiry_time = json_get_or(j, "expiry_time", 0LL);
  tokens_.project_id = json_get_or(j, "project_id", std::string{});
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
  j["project_id"] = tokens.project_id;
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
    return absl::UnauthenticatedError(provider_ == Provider::kGoogle
                                          ? "No refresh token available. Please run ./slop_auth.sh google"
                                          : "No refresh token available. Please run ./slop_auth.sh chatgpt-plus");
  }

  LOG(INFO) << "Refreshing OAuth token...";
  std::string token_url;
  std::string body;
  if (provider_ == Provider::kGoogle) {
    token_url = kGoogleOAuthTokenUrl;
    body = absl::StrCat("refresh_token=", tokens_.refresh_token, "&client_id=", kGeminiClientId,
                        "&client_secret=", kGeminiClientSecret, "&grant_type=refresh_token");
  } else {
    token_url = kOpenAiOAuthTokenUrl;
    const char* client_secret = std::getenv("CHATGPT_CLIENT_SECRET");
    body = absl::StrCat("refresh_token=", tokens_.refresh_token, "&client_id=", kOpenAiOAuthClientId,
                        "&grant_type=refresh_token");
    if (client_secret != nullptr && *client_secret != '\0') {
      absl::StrAppend(&body, "&client_secret=", client_secret);
    }
  }

  auto res = http_client_->Post(token_url, body, {"Content-Type: application/x-www-form-urlencoded"});
  if (!res.ok()) {
    LOG(ERROR) << "Token refresh failed: " << res.status().ToString();
    return absl::UnauthenticatedError(provider_ == Provider::kGoogle
                                          ? "Token refresh failed. Please run ./slop_auth.sh google"
                                          : "Token refresh failed. Please run ./slop_auth.sh chatgpt-plus");
  }
  LOG(INFO) << "Token refreshed successfully.";

  auto j_opt = json_parse(*res);
  if (!j_opt) return absl::InternalError("Failed to parse refresh response");
  const auto& j = *j_opt;

  tokens_.access_token = json_get_or(j, "access_token", std::string{});
  tokens_.expiry_time = absl::ToUnixSeconds(absl::Now()) + json_get_or(j, "expires_in", 3600);
  if (provider_ == Provider::kOpenAi) {
    tokens_.account_id = ExtractOpenAiAccountIdFromJwt(tokens_.access_token);
  }

  return SaveTokens(tokens_);
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

absl::StatusOr<std::string> OAuthHandler::GetProjectId() {
  if (provider_ != Provider::kGoogle) {
    return absl::FailedPreconditionError("Project ID is only available for Google OAuth");
  }
  if (!manual_project_id_.empty()) return manual_project_id_;
  if (!tokens_.project_id.empty()) return tokens_.project_id;

  auto token = GetValidToken();
  if (!token.ok()) return token.status();

  auto disc = DiscoverProjectId(*token);
  if (disc.ok()) {
    tokens_.project_id = *disc;
    (void)SaveTokens(tokens_);
    return *disc;
  }

  return absl::NotFoundError("No project ID found. Use --project to specify one.");
}

std::string OAuthHandler::GetGcpProjectFromGcloud() {
  std::array<char, 128> buffer;
  std::string result;
  std::unique_ptr<FILE, decltype(&pclose)> pipe(popen("gcloud config get-value project 2>/dev/null", "r"), pclose);
  if (!pipe) return "";
  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    result += buffer.data();
  }
  result.erase(result.find_last_not_of(" \n\r\t") + 1);
  return result;
}

absl::StatusOr<std::string> OAuthHandler::DiscoverProjectId(const std::string& access_token) {
  // 1. Try loadCodeAssist (the authoritative way for GCA / Managed Project)
  std::string gca_url = absl::StrCat(kCloudCodeBaseUrl, "/v1internal:loadCodeAssist");

  // GCA identification headers
  std::vector<std::string> headers = {"Authorization: Bearer " + access_token, "Content-Type: application/json",
                                      absl::StrCat("User-Agent: ", kUserAgent),
                                      absl::StrCat("X-Goog-Api-Client: ", kGcaApiClient),
                                      absl::StrCat("Client-Metadata: ", kGcaClientMetadata)};

  nlohmann::json gca_req = {
      {"metadata", {{"ideType", "IDE_UNSPECIFIED"}, {"platform", "PLATFORM_UNSPECIFIED"}, {"pluginType", "GEMINI"}}}};

  const char* env_p = std::getenv("GOOGLE_CLOUD_PROJECT");
  if (!env_p) env_p = std::getenv("GOOGLE_CLOUD_PROJECT_ID");
  if (env_p) {
    gca_req["cloudaicompanionProject"] = env_p;
    gca_req["metadata"]["duetProject"] = env_p;
  }

  auto gca_res =
      http_client_->Post(gca_url, gca_req.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), headers);
  if (gca_res.ok()) {
    auto j_opt = json_parse(*gca_res);
    if (j_opt) {
      const auto* proj = json_at(*j_opt, "cloudaicompanionProject");
      if (proj != nullptr && !proj->is_null()) {
        if (proj->is_string()) {
          std::string pid = json_getter<std::string>::get(*proj).value_or("");
          if (!pid.empty()) return pid;
        }
        if (proj->is_object()) {
          std::string pid = json_get_or(*proj, "id", std::string{});
          if (!pid.empty()) return pid;
        }
      }
    }
  }

  // 2. Try env var fallback
  if (env_p) return std::string(env_p);

  // 3. Try gcloud config
  std::string gcloud_project = GetGcpProjectFromGcloud();
  if (!gcloud_project.empty()) return gcloud_project;

  // 4. Fallback to listing projects
  std::string url = absl::StrCat(kCloudResourceManagerBaseUrl, "/projects");
  auto res = http_client_->Get(url, {"Authorization: Bearer " + access_token});
  if (res.ok()) {
    auto j_opt = json_parse(*res);
    if (j_opt) {
      auto projects = json_get<nlohmann::json::array_t>(*j_opt, "projects");
      if (projects && !projects->empty()) {
        return json_get_or((*projects)[0], "projectId", std::string{});
      }
    }
  }

  return absl::NotFoundError("Could not discover project ID");
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
