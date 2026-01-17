#include "oauth_handler.h"

#include <fstream>
#include <iostream>
#include <sstream>

#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "absl/time/clock.h"
#include <nlohmann/json.hpp>

namespace slop {

namespace {
// Gemini Public
const char* kGeminiClientId = "681255809395-oo8ft2oprdrnp9e3aqf6av3hmdib135j.apps.googleusercontent.com";
const char* kGeminiClientSecret = "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"; 

// Antigravity Internal
const char* kAntigravityClientId = "1071006060591-tmhssin2h21lcre235vtolojh4g403ep.apps.googleusercontent.com";
const char* kAntigravityClientSecret = "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";

std::string GetHomeDir() {
  const char* home = std::getenv("HOME");
  return home ? home : "";
}
} // namespace

OAuthHandler::OAuthHandler(HttpClient* http_client, OAuthMode mode) 
    : http_client_(http_client), mode_(mode) {
  std::string home = GetHomeDir();
  if (!home.empty()) {
    std::string filename = (mode == OAuthMode::ANTIGRAVITY) ? "antigravity_token.json" : "token.json";
    token_path_ = home + "/.config/slop/" + filename;
  }
}

absl::Status OAuthHandler::LoadTokens() {
  if (token_path_.empty()) return absl::NotFoundError("No home directory found");
  std::ifstream f(token_path_);
  if (!f.is_open()) {
      std::string script = (mode_ == OAuthMode::ANTIGRAVITY) ? "./antigravity_auth.sh" : "./gemini_auth.sh";
      return absl::NotFoundError("Token file not found. Please run " + script);
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
  std::string cmd = "mkdir -p " + dir;
  (void)system(cmd.c_str());

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
      std::string script = (mode_ == OAuthMode::ANTIGRAVITY) ? "./antigravity_auth.sh" : "./gemini_auth.sh";
      return absl::UnauthenticatedError("No refresh token available. Please run " + script);
  }

  std::string client_id = (mode_ == OAuthMode::ANTIGRAVITY) ? kAntigravityClientId : kGeminiClientId;
  std::string client_secret = (mode_ == OAuthMode::ANTIGRAVITY) ? kAntigravityClientSecret : kGeminiClientSecret;

  std::string token_url = "https://oauth2.googleapis.com/token";
  std::string body = "refresh_token=" + tokens_.refresh_token +
                     "&client_id=" + client_id +
                     "&client_secret=" + client_secret +
                     "&grant_type=refresh_token";

  auto res = http_client_->Post(token_url, body, {"Content-Type: application/x-www-form-urlencoded"});
  if (!res.ok()) {
      std::string script = (mode_ == OAuthMode::ANTIGRAVITY) ? "./antigravity_auth.sh" : "./gemini_auth.sh";
      return absl::UnauthenticatedError("Token refresh failed. Please run " + script);
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
  std::string gca_url = "https://cloudcode-pa.googleapis.com/v1internal:loadCodeAssist";
  
  // GCA identification headers
  std::vector<std::string> headers = {
      "Authorization: Bearer " + access_token,
      "Content-Type: application/json",
      "X-Goog-Api-Client: google-cloud-sdk vscode_cloudshelleditor/0.1",
      "Client-Metadata: {\"ideType\":\"IDE_UNSPECIFIED\",\"platform\":\"PLATFORM_UNSPECIFIED\",\"pluginType\":\"GEMINI\"}"
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

  // 2. Fallback for Antigravity mode specifically
  if (mode_ == OAuthMode::ANTIGRAVITY) {
      return std::string("rising-fact-p41fc");
  }

  // 3. Try env var fallback
  if (env_p) return std::string(env_p);

  // 4. Try gcloud config
  std::string gcloud_project = GetGcpProjectFromGcloud();
  if (!gcloud_project.empty()) return gcloud_project;

  // 5. Fallback to listing projects
  std::string url = "https://cloudresourcemanager.googleapis.com/v1/projects";
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

  std::string enable_url = "https://serviceusage.googleapis.com/v1/projects/" + project_id + "/services/generativelanguage.googleapis.com:enable";
  (void)http_client_->Post(enable_url, "", {"Authorization: Bearer " + token});
  
  return absl::OkStatus();
}

} // namespace slop
