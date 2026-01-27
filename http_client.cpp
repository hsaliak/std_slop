#include "http_client.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <iostream>
#include <thread>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/strip.h"
#include "nlohmann/json.hpp"
namespace slop {

namespace {
struct CurlDeleter {
  void operator()(CURL* curl) const {
    if (curl) curl_easy_cleanup(curl);
  }
};
struct SlistDeleter {
  void operator()(struct curl_slist* list) const {
    if (list) curl_slist_free_all(list);
  }
};
}  // namespace

HttpClient::HttpClient() { curl_global_init(CURL_GLOBAL_ALL); }

HttpClient::~HttpClient() { curl_global_cleanup(); }

size_t HttpClient::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
  static_cast<std::string*>(userp)->append(static_cast<const char*>(contents), size * nmemb);
  return size * nmemb;
}

size_t HttpClient::HeaderCallback(void* contents, size_t size, size_t nmemb, void* userp) {
  size_t total_size = size * nmemb;
  std::string header(static_cast<char*>(contents), total_size);
  auto* headers = static_cast<absl::flat_hash_map<std::string, std::string>*>(userp);

  size_t colon_pos = header.find(':');
  if (colon_pos != std::string::npos) {
    std::string key = std::string(absl::StripAsciiWhitespace(header.substr(0, colon_pos)));
    std::string value = std::string(absl::StripAsciiWhitespace(header.substr(colon_pos + 1)));
    (*headers)[absl::AsciiStrToLower(key)] = value;
  }

  return total_size;
}

int HttpClient::ProgressCallback(void* clientp, [[maybe_unused]] curl_off_t dltotal, [[maybe_unused]] curl_off_t dlnow,
                                 [[maybe_unused]] curl_off_t ultotal, [[maybe_unused]] curl_off_t ulnow) {
  HttpClient* client = static_cast<HttpClient*>(clientp);
  if (client->abort_requested_) {
    return 1;
  }

  // Check for Esc key (ASCII 27) - minimal impact check
  static auto last_check = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_check).count() < 100) {
    return 0;
  }
  last_check = now;

  if (!isatty(STDIN_FILENO)) {
    return 0;
  }

  struct termios oldt, newt;
  if (tcgetattr(STDIN_FILENO, &oldt) != 0) return 0;
  newt = oldt;
  newt.c_lflag &= ~(ICANON | ECHO);
  if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) != 0) return 0;
  int oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
  if (oldf == -1) {
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return 0;
  }
  if (fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK) == -1) {
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return 0;
  }

  int ch = getchar();

  (void)tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
  (void)fcntl(STDIN_FILENO, F_SETFL, oldf);

  if (ch == 27) {
    std::cout << "\n[Cancelled by user]" << std::endl;
    client->Abort();
    return 1;
  }

  return 0;
}

absl::StatusOr<std::string> HttpClient::Post(const std::string& url, const std::string& body,
                                             const std::vector<std::string>& headers) {
  return ExecuteWithRetry(url, "POST", body, headers);
}

absl::StatusOr<std::string> HttpClient::Get(const std::string& url, const std::vector<std::string>& headers) {
  return ExecuteWithRetry(url, "GET", "", headers);
}

absl::StatusOr<std::string> HttpClient::ExecuteWithRetry(const std::string& url, const std::string& method,
                                                         const std::string& body,
                                                         const std::vector<std::string>& headers) {
  ResetAbort();
  LOG(INFO) << "Executing HTTP " << method << " to " << url;

  int max_retries = 3;
  int retry_count = 0;
  int64_t backoff_ms = 1000;

  std::unique_ptr<CURL, CurlDeleter> curl(curl_easy_init());
  if (!curl) {
    return absl::InternalError("Failed to initialize CURL");
  }

  struct curl_slist* raw_chunk = nullptr;
  for (const auto& header : headers) {
    raw_chunk = curl_slist_append(raw_chunk, header.c_str());
    VLOG(1) << "Header: " << header;
  }
  VLOG(2) << "Request Body: " << body;
  std::unique_ptr<struct curl_slist, SlistDeleter> chunk(raw_chunk);

  while (true) {
    std::string response_string;
    absl::flat_hash_map<std::string, std::string> response_headers;

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    if (method == "POST") {
      curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
    } else {
      curl_easy_setopt(curl.get(), CURLOPT_HTTPGET, 1L);
    }
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, chunk.get());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, HttpClient::WriteCallback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response_string);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, HttpClient::HeaderCallback);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &response_headers);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, HttpClient::ProgressCallback);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 60L);

    CURLcode res = curl_easy_perform(curl.get());

    long response_code = 0;  // NOLINT(runtime/int)
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response_code);

    if (res != CURLE_OK) {
      if (this->abort_requested_) {
        LOG(INFO) << "Request cancelled by user";
        return absl::CancelledError("Request cancelled by user");
      }

      LOG(WARNING) << "CURL error: " << curl_easy_strerror(res) << " (res=" << res << ")";
      if (retry_count < max_retries) {
        LOG(INFO) << "Retrying in " << backoff_ms << "ms... (Attempt " << retry_count + 1 << "/" << max_retries << ")";
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
        retry_count++;
        backoff_ms *= 2;
        continue;
      }
      LOG(ERROR) << "Maximum retries reached for CURL error: " << curl_easy_strerror(res);
      return absl::InternalError("CURL error: " + std::string(curl_easy_strerror(res)));
    }

    LOG(INFO) << "HTTP Status: " << response_code;
    VLOG(2) << "Response Body: " << response_string;

    if (response_code >= 200 && response_code < 300) {
      return response_string;
    }

    LOG(ERROR) << "HTTP error " << response_code << ": " << response_string;

    if (response_code >= 500 || response_code == 429) {
      int64_t retry_after_ms = ParseRetryAfter(response_headers);
      int64_t x_reset_ms = (response_code == 429) ? ParseXRateLimitReset(response_headers) : -1;
      int64_t google_retry_ms = ParseGoogleRetryDelay(response_string);

      int64_t extra_wait = std::max({retry_after_ms, x_reset_ms, google_retry_ms});

      if (retry_count < max_retries) {
        int64_t wait_ms = backoff_ms;
        if (extra_wait > 0) {
          LOG(INFO) << "Server suggested backoff for " << response_code << ": " << extra_wait << "ms";
          wait_ms = std::max(wait_ms, extra_wait);
        }

        LOG(INFO) << "Retrying " << response_code << " in " << wait_ms << "ms... (Attempt " << retry_count + 1 << "/"
                  << max_retries << ")";
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        retry_count++;
        backoff_ms *= 2;
        continue;
      }
      if (extra_wait > 0) {
        LOG(ERROR) << "Maximum retries reached for " << response_code << ". Server still suggesting backoff of "
                   << extra_wait << "ms";
      }
    }

    return absl::Status(static_cast<absl::StatusCode>(response_code),
                        "HTTP error " + std::to_string(response_code) + ": " + response_string);
  }
}

int64_t HttpClient::ParseRetryAfter(const absl::flat_hash_map<std::string, std::string>& headers) {
  auto it = headers.find("retry-after");
  if (it == headers.end()) return -1;

  const std::string& value = it->second;

  // Try parsing as seconds
  int64_t seconds = 0;
  if (absl::SimpleAtoi(value, &seconds)) {
    VLOG(1) << "Parsed Retry-After as seconds: " << seconds;
    return seconds * 1000;
  }

  // Try parsing as HTTP-Date
  absl::Time retry_time;
  std::string err;
  // Standard HTTP date formats (RFC 7231)
  // IMF-fixdate: Fri, 31 Dec 1999 23:59:59 GMT
  if (absl::ParseTime("%a, %d %b %Y %H:%M:%S GMT", value, &retry_time, &err)) {
    int64_t diff_ms = absl::ToInt64Milliseconds(retry_time - absl::Now());
    VLOG(1) << "Parsed Retry-After as date: " << value << " (" << diff_ms << "ms from now)";
    return std::max<int64_t>(0, diff_ms);
  }

  LOG(WARNING) << "Malformed Retry-After header: " << value;
  return -1;
}

int64_t HttpClient::ParseXRateLimitReset(const absl::flat_hash_map<std::string, std::string>& headers) {
  auto it = headers.find("x-ratelimit-reset");
  if (it == headers.end()) return -1;

  const std::string& value = it->second;
  double reset_val = 0;
  if (!absl::SimpleAtod(value, &reset_val)) {
    LOG(WARNING) << "Malformed x-ratelimit-reset header: " << value;
    return -1;
  }

  // If it's a large value, it's likely a Unix timestamp
  if (reset_val > 1000000000) {
    int64_t now_ts = absl::ToUnixSeconds(absl::Now());
    int64_t diff = static_cast<int64_t>(reset_val) - now_ts;
    int64_t wait_ms = std::max<int64_t>(0, diff * 1000);
    VLOG(1) << "Parsed x-ratelimit-reset as timestamp: " << reset_val << " (" << wait_ms << "ms wait)";
    return wait_ms;
  }

  // Otherwise, treat as relative seconds
  return static_cast<int64_t>(reset_val * 1000);
}

int64_t HttpClient::ParseGoogleRetryDelay(const std::string& response_body) {
  auto j = nlohmann::json::parse(response_body, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return -1;

  int64_t max_delay_ms = -1;

  auto update_max_delay = [&](const std::string& delay_str) {
    absl::Duration d;
    if (absl::ParseDuration(delay_str, &d)) {
      max_delay_ms = std::max(max_delay_ms, absl::ToInt64Milliseconds(d));
    }
  };

  if (j.contains("error") && j["error"].is_object()) {
    const auto& error = j["error"];

    // 1. Parse from message
    if (error.contains("message") && error["message"].is_string()) {
      std::string msg = error["message"];
      constexpr absl::string_view kPrefix = "Your quota will reset after ";
      size_t pos = msg.find(kPrefix);
      if (pos != std::string::npos) {
        absl::string_view delay_part = absl::string_view(msg).substr(pos + kPrefix.size());
        delay_part = absl::StripSuffix(delay_part, ".");
        update_max_delay(std::string(delay_part));
      }
    }

    // 2. Parse from details
    if (error.contains("details") && error["details"].is_array()) {
      for (const auto& detail : error["details"]) {
        if (!detail.is_object() || !detail.contains("@type") || !detail["@type"].is_string()) continue;

        std::string type = detail["@type"];
        if (type == "type.googleapis.com/google.rpc.RetryInfo" && detail.contains("retryDelay") &&
            detail["retryDelay"].is_string()) {
          update_max_delay(detail["retryDelay"]);
        } else if (type == "type.googleapis.com/google.rpc.ErrorInfo" && detail.contains("metadata") &&
                   detail["metadata"].is_object()) {
          const auto& metadata = detail["metadata"];
          if (metadata.contains("quotaResetDelay") && metadata["quotaResetDelay"].is_string()) {
            update_max_delay(metadata["quotaResetDelay"]);
          }
        }
      }
    }
  }

  if (max_delay_ms > 0) {
    VLOG(1) << "Parsed Google retry delay: " << max_delay_ms << "ms";
  }

  return max_delay_ms;
}

}  // namespace slop
