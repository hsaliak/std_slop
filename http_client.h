#ifndef SLOP_SQL_HTTP_CLIENT_H_
#define SLOP_SQL_HTTP_CLIENT_H_

#include <atomic>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"

#include <curl/curl.h>
namespace slop {

class HttpClient {
 public:
  HttpClient();
  virtual ~HttpClient();

  // Non-copyable
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  // Sends a POST request to the given URL with the provided JSON body and headers.
  virtual absl::StatusOr<std::string> Post(const std::string& url, const std::string& body,
                                           const std::vector<std::string>& headers);

  virtual absl::StatusOr<std::string> Get(const std::string& url, const std::vector<std::string>& headers);

  void Abort() { abort_requested_ = true; }
  void ResetAbort() { abort_requested_ = false; }

  // Public for testing
  int64_t ParseRetryAfter(const absl::flat_hash_map<std::string, std::string>& headers);
  static size_t HeaderCallback(void* contents, size_t size, size_t nmemb, void* userp);

 private:
  absl::StatusOr<std::string> ExecuteWithRetry(const std::string& url, const std::string& method,
                                               const std::string& body, const std::vector<std::string>& headers);

  static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
  static int ProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                              curl_off_t ulnow);

  std::atomic<bool> abort_requested_{false};
};

}  // namespace slop

#endif  // SLOP_SQL_HTTP_CLIENT_H_
