#ifndef SLOP_SQL_OAUTH_HANDLER_H_
#define SLOP_SQL_OAUTH_HANDLER_H_

#include <memory>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "core/http_client.h"

namespace slop {

struct OAuthTokens {
  std::string access_token;
  std::string refresh_token;
  int64_t expiry_time = 0;  // Epoch time in seconds
  std::string project_id;
  std::string account_id;
};

class OAuthHandler {
 public:
  enum class Provider { kGoogle, kOpenAi };

  explicit OAuthHandler(HttpClient* http_client);
  OAuthHandler(HttpClient* http_client, Provider provider);

  absl::StatusOr<std::string> GetValidToken();
  absl::StatusOr<std::string> GetProjectId();
  absl::StatusOr<std::string> GetOpenAiAccountId();

  void SetProjectId(const std::string& project_id) { manual_project_id_ = project_id; }
  bool IsEnabled() const { return enabled_; }
  void SetEnabled(bool enabled) { enabled_ = enabled; }
  void SetTokenPath(const std::string& token_path) { token_path_ = token_path; }

  std::string GetTokenPath() const { return token_path_; }
  Provider GetProvider() const { return provider_; }

 protected:
  std::string token_path_;

 private:
  absl::Status LoadTokens();
  absl::Status SaveTokens(const OAuthTokens& tokens);
  absl::Status RefreshToken();
  static std::string ExtractOpenAiAccountIdFromJwt(const std::string& jwt);
  std::string GetGcpProjectFromGcloud();
  absl::StatusOr<std::string> DiscoverProjectId(const std::string& access_token);

  HttpClient* http_client_;
  OAuthTokens tokens_;
  bool enabled_ = false;
  std::string manual_project_id_;
  Provider provider_ = Provider::kGoogle;
};

}  // namespace slop

#endif  // SLOP_SQL_OAUTH_HANDLER_H_
