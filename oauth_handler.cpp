#include "oauth_handler.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <random>
#include <sstream>

#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "absl/time/clock.h"
#include <nlohmann/json.hpp>

namespace slop {

namespace {

const char* kClientId = "681255809395-oo8ft2oprdrnp9e3aqf6av3hmdib135j.apps.googleusercontent.com";
const char* kClientSecret = "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"; // https://github.com/google-gemini/gemini-cli/blob/53f54436c9935c357fa0e5d0cbaa6da863b8f7ca/packages/core/src/code_assist/oauth2.ts#L70

std::string GetHomeDir() {
  const char* home = std::getenv("HOME");
  return home ? home : "";
}

std::string GenerateState() {
  static const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  std::default_random_engine rng(std::random_device{}());
  std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);
  std::string state;
  for (int i = 0; i < 32; ++i) state += charset[dist(rng)];
  return state;
}

} // namespace

void OAuthHandler::FdDeleter::operator()(int* fd) const {
  if (fd) {
    if (*fd >= 0) close(*fd);
    delete fd;
  }
}

OAuthHandler::OAuthHandler(HttpClient* http_client) : http_client_(http_client) {
  std::string home = GetHomeDir();
  if (!home.empty()) {
    token_path_ = home + "/.config/slop/tokens.json";
  }
}

absl::Status OAuthHandler::LoadTokens() {
  if (token_path_.empty()) return absl::NotFoundError("No home directory found");
  std::ifstream f(token_path_);
  if (!f.is_open()) return absl::NotFoundError("Token file not found");
  
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
  
  // Ensure directory exists
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

absl::Status OAuthHandler::StartLoginFlow() {
  int raw_server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (raw_server_fd < 0) return absl::InternalError("Failed to create socket");
  ScopedFd server_fd(new int(raw_server_fd));

  struct sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = 0; // Random port

  if (bind(*server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
    return absl::InternalError("Failed to bind");
  }

  socklen_t len = sizeof(address);
  if (getsockname(*server_fd, (struct sockaddr*)&address, &len) < 0) {
    return absl::InternalError("Failed to getsockname");
  }
  int port = ntohs(address.sin_port);

  if (listen(*server_fd, 1) < 0) {
    return absl::InternalError("Failed to listen");
  }

  std::string state = GenerateState();
  std::string redirect_uri = "http://127.0.0.1:" + std::to_string(port);
  
  // Construct OAuth URL
  std::string auth_url = "https://accounts.google.com/o/oauth2/v2/auth?"
                         "client_id=" + std::string(kClientId) +
                         "&redirect_uri=" + redirect_uri +
                         "&response_type=code"
                                                    "&scope=https://www.googleapis.com/auth/cloud-platform%20https://www.googleapis.com/auth/userinfo.email%20https://www.googleapis.com/auth/userinfo.profile"                         "&access_type=offline"
                         "&prompt=consent"
                         "&state=" + state;

  std::cout << "\nTo authenticate, please visit the following URL in your browser:\n\n"
            << auth_url << "\n" << std::endl;

  int raw_new_socket = accept(*server_fd, nullptr, nullptr);
  if (raw_new_socket < 0) {
    return absl::InternalError("Failed to accept connection");
  }
  ScopedFd new_socket(new int(raw_new_socket));

  char buffer[4096] = {0};
  (void)read(*new_socket, buffer, 4096);
  std::string request(buffer);

  // Basic extraction of code and state from GET request
  std::string code;
  std::string received_state;
  size_t code_pos = request.find("code=");
  if (code_pos != std::string::npos) {
    size_t end_pos = request.find('&', code_pos);
    if (end_pos == std::string::npos) end_pos = request.find(' ', code_pos);
    code = request.substr(code_pos + 5, end_pos - (code_pos + 5));
  }
  size_t state_pos = request.find("state=");
  if (state_pos != std::string::npos) {
    size_t end_pos = request.find('&', state_pos);
    if (end_pos == std::string::npos) end_pos = request.find(' ', state_pos);
    received_state = request.substr(state_pos + 6, end_pos - (state_pos + 6));
  }

  std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
                         "<html><body><h1>Success!</h1><p>You can close this window now.</p></body></html>";
  (void)send(*new_socket, response.c_str(), response.length(), 0);

  if (received_state != state) return absl::InternalError("State mismatch (CSRF?)");
  if (code.empty()) return absl::InternalError("No code received");

  // Exchange code for tokens
  std::string token_url = "https://oauth2.googleapis.com/token";
  std::string body = "code=" + code +
                     "&client_id=" + std::string(kClientId) +
                     "&client_secret=" + std::string(kClientSecret) +
                     "&redirect_uri=" + redirect_uri +
                     "&grant_type=authorization_code";

  auto res = http_client_->Post(token_url, body, {"Content-Type: application/x-www-form-urlencoded"});
  if (!res.ok()) return res.status();

  auto j = nlohmann::json::parse(*res, nullptr, false);
  if (j.is_discarded()) return absl::InternalError("Failed to parse token response");

  OAuthTokens new_tokens;
  new_tokens.access_token = j.value("access_token", "");
  new_tokens.refresh_token = j.value("refresh_token", tokens_.refresh_token);
  new_tokens.expiry_time = absl::ToUnixSeconds(absl::Now()) + j.value("expires_in", 3600);

  auto status = SaveTokens(new_tokens);
  if (!status.ok()) return status;

  // Discover/Provision project
  (void)ProvisionProject();

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
  if (tokens_.refresh_token.empty()) return absl::UnauthenticatedError("No refresh token");

  std::string token_url = "https://oauth2.googleapis.com/token";
  std::string body = "refresh_token=" + tokens_.refresh_token +
                     "&client_id=" + std::string(kClientId) +
                     "&client_secret=" + std::string(kClientSecret) +
                     "&grant_type=refresh_token";

  auto res = http_client_->Post(token_url, body, {"Content-Type: application/x-www-form-urlencoded"});
  if (!res.ok()) return res.status();

  auto j = nlohmann::json::parse(*res, nullptr, false);
  if (j.is_discarded()) return absl::InternalError("Failed to parse refresh token response");

  tokens_.access_token = j.value("access_token", "");
  tokens_.expiry_time = absl::ToUnixSeconds(absl::Now()) + j.value("expires_in", 3600);
  
  return SaveTokens(tokens_);
}

std::string OAuthHandler::GetGcpProjectFromGcloud() {
  FILE* pipe = popen("gcloud config get-value project 2>/dev/null", "r");
  if (!pipe) return "";
  char buffer[128];
  std::string result = "";
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    result += buffer;
  }
  pclose(pipe);
  return std::string(absl::StripAsciiWhitespace(result));
}

absl::Status OAuthHandler::ProvisionProject() {
  auto token_or = GetValidToken();
  if (!token_or.ok()) return token_or.status();

  std::vector<std::string> headers = {
    "Authorization: Bearer " + *token_or,
    "Content-Type: application/json",
    "User-Agent: google-api-nodejs-client/9.15.1",
    "X-Goog-Api-Client: gl-node/22.17.0"
  };

  std::string base_url = "https://cloudcode-pa.googleapis.com/v1internal";
  
  // Resolve project preference: Flag > Env > gcloud
  std::string preferred_project = manual_project_id_;
  if (preferred_project.empty()) {
    const char* env_proj = std::getenv("GOOGLE_CLOUD_PROJECT");
    if (!env_proj) env_proj = std::getenv("GOOGLE_CLOUD_PROJECT_ID");
    if (env_proj) preferred_project = env_proj;
  }
  if (preferred_project.empty()) {
    preferred_project = GetGcpProjectFromGcloud();
  }

  nlohmann::json load_req = {
    {"metadata", {
      {"ideType", "IDE_UNSPECIFIED"},
      {"platform", "PLATFORM_UNSPECIFIED"},
      {"pluginType", "GEMINI"},
      {"duetProject", preferred_project}
    }}
  };
  if (!preferred_project.empty()) {
    load_req["cloudaicompanionProject"] = preferred_project;
  }

  auto res = http_client_->Post(base_url + ":loadCodeAssist", load_req.dump(), headers);
  if (!res.ok()) return res.status();

  auto j = nlohmann::json::parse(*res, nullptr, false);
  if (j.is_discarded()) return absl::InternalError("Failed to parse loadCodeAssist response");

  std::string tier_id = "free";
  if (j.contains("allowedTiers") && j["allowedTiers"].is_array()) {
    for (const auto& t : j["allowedTiers"]) {
      if (t.value("isDefault", false)) {
        tier_id = t.value("id", "free");
        break;
      }
    }
  }

  if (j.contains("currentTier")) {
    tier_id = j["currentTier"].value("id", tier_id);
    if (j.contains("cloudaicompanionProject") && j["cloudaicompanionProject"].is_string()) {
      std::string discovered = j["cloudaicompanionProject"];
      if (tier_id == "free") {
        if (!manual_project_id_.empty() && manual_project_id_ != discovered) {
          std::cout << "Note: Google Cloud Free Tier requires using the managed project " << discovered 
                    << " (ignoring manual override " << manual_project_id_ << ")." << std::endl;
        }
        tokens_.project_id = discovered;
        return SaveTokens(tokens_);
      } else {
        if (manual_project_id_.empty()) {
          tokens_.project_id = discovered;
          return SaveTokens(tokens_);
        } else {
          std::cout << "Discovered managed project " << discovered << " but using manual override " << manual_project_id_ << std::endl;
          return absl::OkStatus();
        }
      }
    }
    
    if (!preferred_project.empty()) {
      if (manual_project_id_.empty()) {
        tokens_.project_id = preferred_project;
        (void)SaveTokens(tokens_);
      }
      return absl::OkStatus();
    }
  }

  std::cout << "Onboarding user to Gemini Cloud Code Assist (Tier: " << tier_id << ")..." << std::endl;
  nlohmann::json onboard_req = {
    {"tierId", tier_id},
    {"metadata", {
      {"ideType", "IDE_UNSPECIFIED"},
      {"platform", "PLATFORM_UNSPECIFIED"},
      {"pluginType", "GEMINI"}
    }}
  };
  if (tier_id != "free" && !preferred_project.empty()) {
    onboard_req["cloudaicompanionProject"] = preferred_project;
    onboard_req["metadata"]["duetProject"] = preferred_project;
  }

  res = http_client_->Post(base_url + ":onboardUser", onboard_req.dump(), headers);
  if (!res.ok()) return res.status();

  j = nlohmann::json::parse(*res, nullptr, false);
  if (j.is_discarded()) return absl::InternalError("Failed to parse onboardUser response");

  // Handle LRO
  while (j.contains("done") && !j["done"].get<bool>()) {
    std::string op_name = j["name"];
    std::cout << "Provisioning project... (polling " << op_name << ")" << std::endl;
    absl::SleepFor(absl::Seconds(5));
    res = http_client_->Get("https://cloudcode-pa.googleapis.com/v1internal/" + op_name, headers);
    if (!res.ok()) return res.status();
    j = nlohmann::json::parse(*res, nullptr, false);
    if (j.is_discarded()) return absl::InternalError("Failed to parse operation response");
  }

  if (j.contains("response") && j["response"].contains("cloudaicompanionProject") && 
      j["response"]["cloudaicompanionProject"].contains("id")) {
    std::string discovered = j["response"]["cloudaicompanionProject"]["id"];
    if (manual_project_id_.empty() || tier_id == "free") {
      if (tier_id == "free" && !manual_project_id_.empty() && manual_project_id_ != discovered) {
        std::cout << "Note: Google Cloud Free Tier requires using the managed project " << discovered 
                  << " (ignoring manual override " << manual_project_id_ << ")." << std::endl;
      }
      tokens_.project_id = discovered;
      return SaveTokens(tokens_);
    } else {
      std::cout << "Provisioned managed project " << discovered << " but using manual override " << manual_project_id_ << std::endl;
      return absl::OkStatus();
    }
  }

  if (!preferred_project.empty()) {
    if (manual_project_id_.empty()) {
      tokens_.project_id = preferred_project;
      (void)SaveTokens(tokens_);
    }
    return absl::OkStatus();
  }

  return absl::NotFoundError("Could not discover or provision a Google Cloud project.");
}

absl::StatusOr<std::string> OAuthHandler::GetProjectId() {
  if (!manual_project_id_.empty()) return manual_project_id_;
  if (tokens_.project_id.empty()) {
    auto status = ProvisionProject();
    if (!status.ok()) return status;
  }
  return tokens_.project_id;
}

} // namespace slop