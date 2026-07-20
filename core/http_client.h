#ifndef SLOP_SQL_HTTP_CLIENT_H_
#define SLOP_SQL_HTTP_CLIENT_H_

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include <curl/curl.h>

namespace slop {

struct HttpResponse {
  long status_code = 0;
  std::string body;
  absl::flat_hash_map<std::string, std::string> headers;
};

class HttpClient {
 public:
  using ChunkCallback = std::function<absl::Status(absl::string_view)>;

  HttpClient();
  HttpClient(int max_retries, int64_t initial_backoff_ms);
  virtual ~HttpClient();

  // Non-copyable
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  virtual absl::StatusOr<std::string> Post(const std::string& url, const std::string& body,
                                           const std::vector<std::string>& headers);

  virtual absl::StatusOr<HttpResponse> PostWithResponse(const std::string& url, const std::string& body,
                                                         const std::vector<std::string>& headers);

  // Delivers response body chunks in transport order and returns the complete body.
  virtual absl::StatusOr<std::string> PostStream(const std::string& url, const std::string& body,
                                                 const std::vector<std::string>& headers,
                                                 ChunkCallback on_chunk);

  virtual absl::StatusOr<HttpResponse> PostStreamWithResponse(const std::string& url, const std::string& body,
                                                               const std::vector<std::string>& headers,
                                                               ChunkCallback on_chunk);

  virtual absl::StatusOr<std::string> Get(const std::string& url, const std::vector<std::string>& headers);
  static bool IsTerminalError(long response_code, const std::string& response_body);
  // Classifies provider context-limit responses for accordion retry.
  static absl::Status ContextOverflowStatus(long response_code, absl::string_view response_body);

  void Abort();
  bool IsAborted() const { return abort_generation_.load() != active_generation_.load(); }

  // Callbacks and internal parsing (public for testing)
  static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
  static int ProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                              curl_off_t ulnow);

  // Parses an HTTP status line (e.g. "HTTP/1.1 200 OK") and writes the numeric
  // status code. Returns false for non-status-line or malformed input.
  static bool ParseHttpStatusLine(absl::string_view header, long* response_code);
  // Captures a single "Key: Value" header line into the lowercase-keyed map.
  static void CaptureHeaderField(absl::string_view header,
                                 absl::flat_hash_map<std::string, std::string>* headers);

  int64_t ParseRetryAfter(const absl::flat_hash_map<std::string, std::string>& headers);
  int64_t ParseXRateLimitReset(const absl::flat_hash_map<std::string, std::string>& headers);
  int64_t ParseGoogleRetryDelay(const std::string& body);

 private:
  void CancellableSleep(int64_t wait_ms);

  absl::StatusOr<HttpResponse> ExecuteWithRetryResponse(const std::string& url, const std::string& method,
                                                         const std::string& body,
                                                         const std::vector<std::string>& headers,
                                                         ChunkCallback on_chunk = nullptr,
                                                         bool return_auth_error_response = false);

  std::atomic<uint64_t> abort_generation_{0};
  std::atomic<uint64_t> active_generation_{0};
  absl::Mutex abort_mutex_;
  absl::CondVar abort_cv_;

  int max_retries_;
  int64_t initial_backoff_ms_;
};

}  // namespace slop

#endif  // SLOP_SQL_HTTP_CLIENT_H_
