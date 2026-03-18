#include "core/http_client.h"

#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>

#include "absl/base/call_once.h"
#include "absl/cleanup/cleanup.h"
#include "absl/log/log.h"
#include "absl/random/random.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "nlohmann/json.hpp"

#include "core/json_utils.h"

namespace slop {

namespace {
absl::once_flag g_curl_init_once;
bool g_curl_init_ok = false;

void EnsureCurlGlobalInit() {
  absl::call_once(g_curl_init_once, []() {
    g_curl_init_ok = (curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK);
    if (!g_curl_init_ok) {
      LOG(ERROR) << "curl_global_init failed";
    }
  });
}
}  // namespace

// Helper to check SLOP_DEBUG_HTTP environment variable
inline bool IsDebugHttpEnabled() {
  static bool enabled = (getenv("SLOP_DEBUG_HTTP") != nullptr);
  return enabled;
}

HttpClient::HttpClient() : max_retries_(5), initial_backoff_ms_(5000) { EnsureCurlGlobalInit(); }

HttpClient::HttpClient(int max_retries, int64_t initial_backoff_ms)
    : max_retries_(max_retries), initial_backoff_ms_(initial_backoff_ms) {
  EnsureCurlGlobalInit();
}

HttpClient::~HttpClient() = default;

size_t HttpClient::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
  static_cast<std::string*>(userp)->append(static_cast<char*>(contents), size * nmemb);
  return size * nmemb;
}

size_t HttpClient::HeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
  size_t total_size = size * nitems;
  std::string header(buffer, total_size);
  auto* headers = static_cast<absl::flat_hash_map<std::string, std::string>*>(userdata);

  size_t colon_pos = header.find(':');
  if (colon_pos != std::string::npos) {
    std::string key = std::string(absl::StripAsciiWhitespace(header.substr(0, colon_pos)));
    std::string value = std::string(absl::StripAsciiWhitespace(header.substr(colon_pos + 1)));

    for (char& c : key) c = std::tolower(c);
    (*headers)[key] = value;
  }

  return total_size;
}

int HttpClient::ProgressCallback(void* clientp, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/, curl_off_t /*ultotal*/,
                                 curl_off_t /*ulnow*/) {
  auto* client = static_cast<HttpClient*>(clientp);
  if (client->IsAborted()) {
    return 1;
  }
  return 0;
}

absl::StatusOr<std::string> HttpClient::Get(const std::string& url, const std::vector<std::string>& headers) {
  return ExecuteWithRetry(url, "GET", "", headers);
}

absl::StatusOr<std::string> HttpClient::Post(const std::string& url, const std::string& body,
                                             const std::vector<std::string>& headers) {
  return ExecuteWithRetry(url, "POST", body, headers);
}

absl::StatusOr<std::string> HttpClient::ExecuteWithRetry(const std::string& url, const std::string& method,
                                                         const std::string& body,
                                                         const std::vector<std::string>& headers) {
  {
    absl::MutexLock lock(abort_mutex_);
    abort_requested_.store(false);
  }
  int retry_count = 0;
  int64_t backoff_ms = initial_backoff_ms_;

  while (retry_count <= max_retries_) {
    CURL* curl = curl_easy_init();
    absl::Cleanup curl_cleaner = [curl] { curl_easy_cleanup(curl); };
    if (!curl) {
      return absl::InternalError("Failed to initialize CURL");
    }

    std::string response_body;
    absl::flat_hash_map<std::string, std::string> response_headers;
    struct curl_slist* chunk = nullptr;
    absl::Cleanup chunk_cleaner = [&chunk] { curl_slist_free_all(chunk); };

    if (IsDebugHttpEnabled()) {
      LOG(INFO) << "HTTP " << method << " to " << url;
      for (const auto& header : headers) {
        if (absl::StartsWithIgnoreCase(header, "Authorization:")) {
          LOG(INFO) << "  Request Header: Authorization: [REDACTED]";
        } else {
          LOG(INFO) << "  Request Header: " << header;
        }
      }
      // only enable when absolutely needed. commented in case it is.
      // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    }

    for (const auto& header : headers) {
      chunk = curl_slist_append(chunk, header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    if (method == "POST") {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    if (IsDebugHttpEnabled()) {
      if (res != CURLE_OK) {
        LOG(INFO) << "CURL error: " << curl_easy_strerror(res) << " (code " << res << ") for " << url;
      } else {
        LOG(INFO) << "HTTP response: " << response_code << " for " << url;
        if (response_body.size() < 1000) {
          LOG(INFO) << "Response body: " << response_body;
        } else {
          LOG(INFO) << "Response body: " << response_body.substr(0, 1000) << "... [truncated]";
        }
      }
    }

    if (res == CURLE_OK) {
      if (response_code >= 200 && response_code < 300) {
        return response_body;
      }

      if (IsTerminalError(response_code, response_body)) {
        if (IsDebugHttpEnabled()) {
          LOG(WARNING) << "Terminal HTTP error detected: " << response_code << " for " << url;
        }
        return absl::UnavailableError(absl::StrCat("Terminal HTTP error: ", response_code, " Body: ", response_body));
      }
    }

    if (retry_count >= max_retries_ || IsAborted()) {
      return absl::UnavailableError(
          absl::StrCat("HTTP request failed after ", retry_count, " retries. Code: ", response_code));
    }

    int64_t header_delay = ParseRetryAfter(response_headers);
    if (header_delay == -1) {
      header_delay = ParseXRateLimitReset(response_headers);
    }
    if (header_delay == -1) {
      header_delay = ParseGoogleRetryDelay(response_body);
    }

    static absl::BitGen bitgen;
    double jitter = absl::Uniform(bitgen, 0.8, 1.2);
    int64_t jittered_backoff = static_cast<int64_t>(backoff_ms * jitter);
    int64_t wait_ms = jittered_backoff;
    if (header_delay > 0) {
      wait_ms = std::max(wait_ms, header_delay);
    }

    LOG(INFO) << "Request failed (code " << response_code << "), retrying in " << wait_ms << "ms... (attempt "
              << (retry_count + 1) << "/" << max_retries_ << ")" << std::endl;

    CancellableSleep(wait_ms);

    if (header_delay <= 0) {
      backoff_ms *= 2;
    }
    retry_count++;
  }

  return absl::UnavailableError("HTTP request failed");
}

void HttpClient::Abort() {
  {
    absl::MutexLock lock(abort_mutex_);
    abort_requested_.store(true);
  }
  abort_cv_.SignalAll();
}

void HttpClient::CancellableSleep(int64_t wait_ms) {
  absl::MutexLock lock(abort_mutex_);
  auto deadline = absl::Now() + absl::Milliseconds(wait_ms);
  while (!abort_requested_.load() && absl::Now() < deadline) {
    abort_cv_.WaitWithDeadline(&abort_mutex_, deadline);
  }
}

bool HttpClient::IsTerminalError(long response_code, const std::string& response_body) {
  // Non-retryable client errors:
  // 400 Bad Request: Request is invalid.
  // 401 Unauthorized: API key missing or invalid.
  // 403 Forbidden: Permission denied or invalid key.
  // 404 Not Found: Model or endpoint not found.
  if (response_code == 400 || response_code == 401 || response_code == 403 || response_code == 404) {
    return true;
  }

  // 429 Too Many Requests: Only terminal if quota is strictly exhausted.
  if (response_code == 429) {
    if (absl::StrContains(response_body, "QUOTA_EXHAUSTED")) {
      return true;
    }
  }

  return false;
}

int64_t HttpClient::ParseRetryAfter(const absl::flat_hash_map<std::string, std::string>& headers) {
  auto it = headers.find("retry-after");
  if (it == headers.end()) return -1;
  const std::string& val = it->second;
  if (val.empty()) return -1;

  char* end;
  long long seconds = std::strtoll(val.c_str(), &end, 10);
  if (end == val.c_str() + val.size()) {
    return seconds * 1000;
  }

  absl::Time time;
  std::string err;
  if (absl::ParseTime("%a, %d %b %Y %H:%M:%S GMT", val, absl::UTCTimeZone(), &time, &err)) {
    auto now = absl::Now();
    if (time > now) {
      return absl::ToInt64Milliseconds(time - now);
    }
  }
  return -1;
}

int64_t HttpClient::ParseXRateLimitReset(const absl::flat_hash_map<std::string, std::string>& headers) {
  auto it = headers.find("x-ratelimit-reset");
  if (it == headers.end()) return -1;
  char* end;
  double reset_val = std::strtod(it->second.c_str(), &end);
  if (end != it->second.c_str()) {
    if (reset_val > 1000000000) {
      auto now = absl::ToUnixSeconds(absl::Now());
      return std::max<int64_t>(0, (static_cast<int64_t>(reset_val) - now) * 1000);
    }
    return static_cast<int64_t>(reset_val * 1000);
  }
  return -1;
}

int64_t HttpClient::ParseGoogleRetryDelay(const std::string& body) {
  auto j_opt = json_parse(body);
  if (!j_opt) return -1;
  const auto& j = *j_opt;

  const nlohmann::json* error = json_at(j, "error");
  if (!error) {
    if (const auto* resp = json_at(j, "response")) {
      error = json_at(*resp, "error");
    }
  }

  if (!error || !error->is_object()) return -1;

  if (auto details = json_get<nlohmann::json::array_t>(*error, "details")) {
    for (const auto& detail : *details) {
      auto type = json_get<std::string>(detail, "@type");
      if (!type) continue;

      if (*type == "type.googleapis.com/google.rpc.RetryInfo") {
        if (auto delay_str = json_get<std::string>(detail, "retryDelay")) {
          if (!delay_str->empty() && delay_str->back() == 's') {
            char* end;
            double d = std::strtod(delay_str->substr(0, delay_str->size() - 1).c_str(), &end);
            return static_cast<int64_t>(d * 1000);
          }
        }
      } else if (*type == "type.googleapis.com/google.rpc.ErrorInfo") {
        if (const auto* metadata = json_at(detail, "metadata")) {
          if (auto delay_str = json_get<std::string>(*metadata, "quotaResetDelay")) {
            if (!delay_str->empty() && delay_str->back() == 's') {
              char* end;
              double d = std::strtod(delay_str->substr(0, delay_str->size() - 1).c_str(), &end);
              return static_cast<int64_t>(d * 1000);
            }
          }
        }
      }
    }
  }

  if (auto message = json_get<std::string>(*error, "message")) {
    size_t after_pos = message->find("after ");
    if (after_pos != std::string::npos) {
      size_t search_start = after_pos + 6;
      size_t s_pos = message->find("s", search_start);
      if (s_pos != std::string::npos) {
        std::string delay_str = message->substr(search_start, s_pos - search_start);
        char* end;
        long long val = std::strtoll(delay_str.c_str(), &end, 10);
        if (end != delay_str.c_str()) {
          return val * 1000;
        }
      }
    }
  }

  return -1;
}

}  // namespace slop