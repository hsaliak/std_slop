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
  ScopedFd server_fd(raw_server_fd);

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
  ScopedFd new_socket(raw_new_socket);

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
  if (tokens_.refresh_token.empty()) {
    return StartLoginFlow();
  }

  std::string token_url = "https://oauth2.googleapis.com/token";
  std::string body = "refresh_token=" + tokens_.refresh_token +
                     "&client_id=" + std::string(kClientId) +
                     "&client_secret=" + std::string(kClientSecret) +
                     "&grant_type=refresh_token";

  auto res = http_client_->Post(token_url, body, {"Content-Type: application/x-www-form-urlencoded"});
  if (!res.ok()) return res.status();

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

  return absl::NotFoundError("No project ID found. Use --project_id to specify one.");
}

std::string OAuthHandler::GetGcpProjectFromGcloud() {
  std::array<char, 128> buffer;
  std::string result;
  std::unique_ptr<FILE, decltype(&pclose)> pipe(popen("gcloud config get-value project 2>/dev/null", "r"), pclose);
  if (!pipe) return "";
  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    result += buffer.data();
  }
  // Trim result
  result.erase(result.find_last_not_of(" \n\r\t") + 1);
  return result;
}

absl::StatusOr<std::string> OAuthHandler::DiscoverProjectId(const std::string& access_token) {
  // 1. Try gcloud
  std::string gcloud_project = GetGcpProjectFromGcloud();
  if (!gcloud_project.empty()) return gcloud_project;

  // 2. List projects via API
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

  // Enable Generative Language API
  std::cout << "Ensuring Generative Language API is enabled for project: " << project_id << "..." << std::endl;
  std::string enable_url = "https://serviceusage.googleapis.com/v1/projects/" + project_id + "/services/generativelanguage.googleapis.com:enable";
  auto res = http_client_->Post(enable_url, "", {"Authorization: Bearer " + token});
  
  if (res.ok()) {
    std::cout << "Generative Language API checked/enabled." << std::endl;
  } else {
    std::cerr << "Warning: Failed to enable Generative Language API: " << res.status() << std::endl;
  }

  return absl::OkStatus();
}

} // namespace slop
