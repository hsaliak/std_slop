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
};

class OAuthHandler {
 public:
  explicit OAuthHandler(HttpClient* http_client);

  absl::StatusOr<std::string> GetValidToken();
  absl::StatusOr<std::string> GetProjectId();
  absl::Status ProvisionProject();

  void SetProjectId(const std::string& project_id) { manual_project_id_ = project_id; }
  bool IsEnabled() const { return enabled_; }
  void SetEnabled(bool enabled) { enabled_ = enabled; }

  std::string GetTokenPath() const { return token_path_; }

 protected:
  std::string token_path_;

 private:
  absl::Status LoadTokens();
  absl::Status SaveTokens(const OAuthTokens& tokens);
  absl::Status RefreshToken();
  std::string GetGcpProjectFromGcloud();
  absl::StatusOr<std::string> DiscoverProjectId(const std::string& access_token);

  HttpClient* http_client_;
  OAuthTokens tokens_;
  bool enabled_ = false;
  std::string manual_project_id_;
};

}  // namespace slop

#endif  // SLOP_SQL_OAUTH_HANDLER_H_
