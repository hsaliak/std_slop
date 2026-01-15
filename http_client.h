#ifndef SENTINEL_SQL_HTTP_CLIENT_H_
#define SENTINEL_SQL_HTTP_CLIENT_H_

#include <string>
#include <vector>
#include <curl/curl.h>
#include "absl/status/statusor.h"

#include <atomic>

namespace sentinel {

class HttpClient {
 public:
  HttpClient();
  ~HttpClient();

  // Non-copyable
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  // Sends a POST request to the given URL with the provided JSON body and headers.
  absl::StatusOr<std::string> Post(const std::string& url,
                                   const std::string& body,
                                   const std::vector<std::string>& headers);

  absl::StatusOr<std::string> Get(const std::string& url,
                                  const std::vector<std::string>& headers);

  void Abort() { abort_requested_ = true; }
  void ResetAbort() { abort_requested_ = false; }

 private:
  static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
  static int ProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);

  std::atomic<bool> abort_requested_{false};
};

}  // namespace sentinel

#endif  // SENTINEL_SQL_HTTP_CLIENT_H_
