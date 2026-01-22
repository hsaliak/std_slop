#include "oauth_handler.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/time/clock.h"
#include "constants.h"
#include "nlohmann/json.hpp"
namespace slop {

namespace {
const char* kGeminiClientId = "681255809395-oo8ft2oprdrnp9e3aqf6av3hmdib135j.apps.googleusercontent.com";
const char* kGeminiClientSecret = "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";

std::string GetHomeDir() {
  const char* home = std::getenv("HOME");
  return home ? home : "";
}
} // namespace
absl::Status MaybeCreateDirectory(const std::string& dir_path) {
  std::error_code ec;
  if (!std::filesystem::create_directories(dir_path, ec) && ec) {
    return absl::InternalError(ec.message());
  }

  return absl::OkStatus();
}

OAuthHandler::OAuthHandler(HttpClient* http_client)
    : http_client_(http_client) {
  std::string home = GetHomeDir();
  if (!home.empty()) {
    token_path_ = home + "/.config/slop/token.json";
  }
}

absl::Status OAuthHandler::LoadTokens() {
  if (token_path_.empty()) return absl::NotFoundError("No home directory found");
  std::ifstream f(token_path_);
  if (!f.is_open()) {
      return absl::NotFoundError("Token file not found. Please run ./slop_auth.sh");
  }

  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto j = nlohmann::json::parse(content, nullptr, false);
  if (j.is_discarded()) {
    return absl::InternalError("Failed to parse tokens");
  }

  tokens_.access_token = j.value("access_token", "");
  tokens_.refresh_token = j.value("refresh_token", "");
  tokens_.expiry_time = j.value("expiry_time", 0LL);
  tokens_.project_id = j.value("project_id", "");
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

  std::ofstream f(token_path_);
  if (!f.is_open()) return absl::InternalError("Failed to open token file for writing");
  f << j.dump(4);
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
      return absl::UnauthenticatedError("No refresh token available. Please run ./slop_auth.sh");
  }

  std::string token_url = kGoogleOAuthTokenUrl;
  std::string body = "refresh_token=" + tokens_.refresh_token +
                     "&client_id=" + std::string(kGeminiClientId) +
                     "&client_secret=" + std::string(kGeminiClientSecret) +
                     "&grant_type=refresh_token";

  auto res = http_client_->Post(token_url, body, {"Content-Type: application/x-www-form-urlencoded"});
  if (!res.ok()) {
      return absl::UnauthenticatedError("Token refresh failed. Please run ./slop_auth.sh");
  }

  auto j = nlohmann::json::parse(*res, nullptr, false);
  if (j.is_discarded()) return absl::InternalError("Failed to parse refresh response");

  tokens_.access_token = j.value("access_token", "");
  tokens_.expiry_time = absl::ToUnixSeconds(absl::Now()) + j.value("expires_in", 3600);

  return SaveTokens(tokens_);
}

absl::StatusOr<std::string> OAuthHandler::GetProjectId() {
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
  std::vector<std::string> headers = {
      "Authorization: Bearer " + access_token,
      "Content-Type: application/json",
      absl::StrCat("User-Agent: ", kGcaUserAgent),
      absl::StrCat("X-Goog-Api-Client: ", kGcaApiClient),
      absl::StrCat("Client-Metadata: ", kGcaClientMetadata)
  };

  nlohmann::json gca_req = {
      {"metadata", {
          {"ideType", "IDE_UNSPECIFIED"},
          {"platform", "PLATFORM_UNSPECIFIED"},
          {"pluginType", "GEMINI"}
      }}
  };

  const char* env_p = std::getenv("GOOGLE_CLOUD_PROJECT");
  if (!env_p) env_p = std::getenv("GOOGLE_CLOUD_PROJECT_ID");
  if (env_p) {
      gca_req["cloudaicompanionProject"] = env_p;
      gca_req["metadata"]["duetProject"] = env_p;
  }

  auto gca_res = http_client_->Post(gca_url, gca_req.dump(), headers);
  if (gca_res.ok()) {
    auto j = nlohmann::json::parse(*gca_res, nullptr, false);
    if (!j.is_discarded() && j.contains("cloudaicompanionProject") && !j["cloudaicompanionProject"].is_null()) {
      auto& proj = j["cloudaicompanionProject"];
      if (proj.is_string()) {
          std::string pid = proj.get<std::string>();
          if (!pid.empty()) return pid;
      }
      if (proj.is_object() && proj.contains("id")) {
          std::string pid = proj["id"].get<std::string>();
          if (!pid.empty()) return pid;
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
    auto j = nlohmann::json::parse(*res, nullptr, false);
    if (!j.is_discarded() && j.contains("projects") && !j["projects"].empty()) {
      return j["projects"][0].value("projectId", "");
    }
  }

  return absl::NotFoundError("Could not discover project ID");
}

absl::Status OAuthHandler::ProvisionProject() {
  auto project_id_res = GetProjectId();
  if (!project_id_res.ok()) return project_id_res.status();
  std::string project_id = *project_id_res;

  auto token_res = GetValidToken();
  if (!token_res.ok()) return token_res.status();
  std::string token = *token_res;

  std::string enable_url = absl::StrCat(kServiceUsageBaseUrl, "/projects/", project_id, "/services/generativelanguage.googleapis.com:enable");
  (void)http_client_->Post(enable_url, "", {"Authorization: Bearer " + token});

  return absl::OkStatus();
}

} // namespace slop
