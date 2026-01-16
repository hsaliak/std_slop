#ifndef SLOP_SQL_OAUTH_HANDLER_H_
#define SLOP_SQL_OAUTH_HANDLER_H_

#include <string>
#include <vector>
#include <memory>
#include <unistd.h>
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "http_client.h"

namespace slop {

struct OAuthTokens {
  std::string access_token;
  std::string refresh_token;
  long long expiry_time = 0; // Epoch time in seconds
  std::string project_id;
};

class OAuthHandler {
 public:
  explicit OAuthHandler(HttpClient* http_client);

  absl::Status StartLoginFlow();
  absl::StatusOr<std::string> GetValidToken();
  absl::StatusOr<std::string> GetProjectId();
  absl::Status ProvisionProject();

  void SetProjectId(const std::string& project_id) { manual_project_id_ = project_id; }
  bool IsEnabled() const { return enabled_; }
  void SetEnabled(bool enabled) { enabled_ = enabled; }

 private:
  absl::Status LoadTokens();
  absl::Status SaveTokens(const OAuthTokens& tokens);
  absl::Status RefreshToken();
  std::string GetGcpProjectFromGcloud();
  absl::StatusOr<std::string> DiscoverProjectId(const std::string& access_token);

  // RAII wrapper for file descriptors to avoid raw new/delete and ensure closing.
  class ScopedFd {
   public:
    explicit ScopedFd(int fd) : fd_(fd) {}
    ~ScopedFd() { if (fd_ >= 0) close(fd_); }
    int operator*() const { return fd_; }
    int get() const { return fd_; }
    
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    ScopedFd& operator=(ScopedFd&& other) noexcept {
      if (this != &other) {
        if (fd_ >= 0) close(fd_);
        fd_ = other.fd_;
        other.fd_ = -1;
      }
      return *this;
    }

   private:
    int fd_;
  };

  HttpClient* http_client_;
  OAuthTokens tokens_;
  bool enabled_ = false;
  std::string token_path_;
  std::string manual_project_id_;
};

} // namespace slop

#endif // SLOP_SQL_OAUTH_HANDLER_H_
