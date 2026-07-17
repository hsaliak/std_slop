#include "core/http_client.h"

#include <chrono>
#include <cctype>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include "absl/base/call_once.h"
#include "absl/cleanup/cleanup.h"
#include "absl/log/log.h"
#include "absl/random/random.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "nlohmann/json.hpp"

#include "core/json_utils.h"

namespace slop {

namespace {
absl::once_flag g_curl_init_once;
bool g_curl_init_ok = false;

// Context-limit parsing is exposed through HttpClient::ContextOverflowStatus for tests.

// Use an idle/low-speed timeout instead of a total wall-clock timeout so
// stream-required APIs can run as long as bytes continue to arrive.
constexpr long kHttpLowSpeedLimitBytesPerSecond = 1L;
constexpr long kHttpLowSpeedTimeSeconds = 360L;

struct ResponseBuffer {
  std::string body;
  size_t bytes_received = 0;
  size_t bytes_delivered = 0;
  long response_code = 0;
  HttpClient::ChunkCallback on_chunk;
  absl::Status callback_status;
};

struct HeaderCapture {
  absl::flat_hash_map<std::string, std::string>* headers;
  ResponseBuffer* response;
};

std::string HttpFailureDetails(CURLcode result, long response_code, size_t bytes_received,
                               absl::Duration elapsed) {
  return absl::StrCat("curl_code=", static_cast<int>(result), " curl_error=\"", curl_easy_strerror(result),
                      "\" response_code=", response_code, " bytes_received=", bytes_received,
                      " elapsed=", absl::FormatDuration(elapsed),
                      " low_speed_limit_bytes_per_second=", kHttpLowSpeedLimitBytesPerSecond,
                      " low_speed_time_seconds=", kHttpLowSpeedTimeSeconds);
}

void EnsureCurlGlobalInit() {
  absl::call_once(g_curl_init_once, []() {
    g_curl_init_ok = (curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK);
    if (!g_curl_init_ok) {
      LOG(ERROR) << "curl_global_init failed";
    }
  });
}

size_t HeaderAndStatusCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
  const size_t total_size = size * nitems;
  const absl::string_view header(buffer, total_size);
  auto* capture = static_cast<HeaderCapture*>(userdata);

  long response_code = 0;
  if (HttpClient::ParseHttpStatusLine(header, &response_code)) {
    capture->response->response_code = response_code;
    capture->headers->clear();
  } else {
    HttpClient::CaptureHeaderField(header, capture->headers);
  }

  return total_size;
}
}  // namespace

// Helper to check SLOP_DEBUG_HTTP environment variable
inline bool IsDebugHttpEnabled() {
  static bool enabled = (getenv("SLOP_DEBUG_HTTP") != nullptr);
  return enabled;
}

std::string FormatResponseHeaders(const absl::flat_hash_map<std::string, std::string>& headers) {
  if (headers.empty()) return "<none>";
  std::vector<std::string> parts;
  parts.reserve(headers.size());
  for (const auto& [key, value] : headers) {
    parts.push_back(absl::StrCat(key, "=", value));
  }
  return absl::StrJoin(parts, " ");
}

void LogStreamingHttpFailure(const std::string& method, const std::string& url, CURLcode res, long response_code,
                             size_t bytes_received, absl::Duration elapsed, const ResponseBuffer& response,
                             const absl::flat_hash_map<std::string, std::string>& response_headers) {
  if (!IsDebugHttpEnabled()) return;
  LOG(WARNING) << "Streaming HTTP request failed for " << method << " " << url << ": "
               << HttpFailureDetails(res, response_code, bytes_received, elapsed);
  if (response.body.size() < 1000) {
    LOG(WARNING) << "Partial response body (" << response.body.size() << " bytes): " << response.body;
  } else {
    LOG(WARNING) << "Partial response body (" << response.body.size() << " bytes): " << response.body.substr(0, 1000)
                 << "... [truncated]";
  }
  LOG(WARNING) << "Response headers: " << FormatResponseHeaders(response_headers);
}

HttpClient::HttpClient() : max_retries_(5), initial_backoff_ms_(5000) { EnsureCurlGlobalInit(); }

HttpClient::HttpClient(int max_retries, int64_t initial_backoff_ms)
    : max_retries_(max_retries), initial_backoff_ms_(initial_backoff_ms) {
  EnsureCurlGlobalInit();
}

HttpClient::~HttpClient() = default;

size_t HttpClient::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
  const size_t total_size = size * nmemb;
  auto* response = static_cast<ResponseBuffer*>(userp);
  response->body.append(static_cast<char*>(contents), total_size);
  response->bytes_received += total_size;

  const bool should_deliver_chunk =
      response->on_chunk && (response->response_code == 0 || (response->response_code >= 200 && response->response_code < 300));
  if (should_deliver_chunk) {
    response->callback_status = response->on_chunk(absl::string_view(static_cast<char*>(contents), total_size));
    if (!response->callback_status.ok()) return 0;
    response->bytes_delivered += total_size;
  }
  return total_size;
}

bool HttpClient::ParseHttpStatusLine(absl::string_view header, long* response_code) {
  header = absl::StripAsciiWhitespace(header);
  if (!absl::StartsWith(header, "HTTP/")) return false;

  const size_t first_space = header.find(' ');
  if (first_space == absl::string_view::npos) return false;
  header.remove_prefix(first_space + 1);
  const size_t second_space = header.find(' ');
  const absl::string_view code_text = header.substr(0, second_space);

  int parsed_code = 0;
  if (!absl::SimpleAtoi(code_text, &parsed_code)) return false;
  *response_code = parsed_code;
  return true;
}

void HttpClient::CaptureHeaderField(absl::string_view header,
                                    absl::flat_hash_map<std::string, std::string>* headers) {
  const size_t colon_pos = header.find(':');
  if (colon_pos == absl::string_view::npos) return;

  std::string key = std::string(absl::StripAsciiWhitespace(header.substr(0, colon_pos)));
  std::string value = std::string(absl::StripAsciiWhitespace(header.substr(colon_pos + 1)));

  for (char& c : key) c = std::tolower(static_cast<unsigned char>(c));
  (*headers)[key] = value;
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

absl::StatusOr<std::string> HttpClient::PostStream(const std::string& url, const std::string& body,
                                                   const std::vector<std::string>& headers,
                                                   ChunkCallback on_chunk) {
  return ExecuteWithRetry(url, "POST", body, headers, std::move(on_chunk));
}

absl::StatusOr<std::string> HttpClient::ExecuteWithRetry(const std::string& url, const std::string& method,
                                                         const std::string& body,
                                                         const std::vector<std::string>& headers,
                                                         ChunkCallback on_chunk) {
  // Preserve an abort issued before the worker starts instead of accepting it
  // as the request's initial generation. Reset after this request so the next
  // independent request is not poisoned by an earlier cancellation.
  absl::Cleanup reset_abort_state = [this] { active_generation_.store(abort_generation_.load()); };
  if (IsAborted()) return absl::CancelledError("HTTP request cancelled before start");
  int retry_count = 0;
  int64_t backoff_ms = initial_backoff_ms_;

  while (retry_count <= max_retries_) {
    CURL* curl = curl_easy_init();
    absl::Cleanup curl_cleaner = [curl] { curl_easy_cleanup(curl); };
    if (!curl) {
      return absl::InternalError("Failed to initialize CURL");
    }

    ResponseBuffer response;
    response.on_chunk = on_chunk;
    absl::flat_hash_map<std::string, std::string> response_headers;
    HeaderCapture header_capture{&response_headers, &response};
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
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderAndStatusCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_capture);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, kHttpLowSpeedLimitBytesPerSecond);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, kHttpLowSpeedTimeSeconds);

    if (method == "POST") {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    }

    const absl::Time start_time = absl::Now();
    CURLcode res = curl_easy_perform(curl);
    const absl::Duration elapsed = absl::Now() - start_time;
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    if (IsAborted()) return absl::CancelledError("HTTP request cancelled");

    if (!response.callback_status.ok()) return response.callback_status;
    // Retrying after delivering any chunk would duplicate stream data for the caller.
    if (on_chunk && response.bytes_delivered > 0 && (res != CURLE_OK || response_code < 200 || response_code >= 300)) {
      LogStreamingHttpFailure(method, url, res, response_code, response.bytes_received, elapsed, response,
                              response_headers);
      return absl::UnavailableError(absl::StrCat("Streaming HTTP request failed after receiving response data. ",
                                                 HttpFailureDetails(res, response_code, response.bytes_received, elapsed)));
    }

    if (IsDebugHttpEnabled()) {
      if (res != CURLE_OK) {
        LOG(INFO) << "CURL error for " << method << " " << url << ": "
                  << HttpFailureDetails(res, response_code, response.bytes_received, elapsed);
      } else {
        LOG(INFO) << "HTTP response: " << response_code << " for " << url
                  << " bytes_received=" << response.bytes_received << " elapsed=" << absl::FormatDuration(elapsed);
        LOG(INFO) << "Response headers: " << FormatResponseHeaders(response_headers);
        if (response.body.size() < 1000) {
          LOG(INFO) << "Response body: " << response.body;
        } else {
          LOG(INFO) << "Response body: " << response.body.substr(0, 1000) << "... [truncated]";
        }
      }
    }

    if (res == CURLE_OK) {
      if (response_code >= 200 && response_code < 300) {
        return response.body;
      }

      if (IsTerminalError(response_code, response.body)) {
        if (IsDebugHttpEnabled()) {
          LOG(WARNING) << "Terminal HTTP error detected for " << method << " " << url << ": "
                       << HttpFailureDetails(res, response_code, response.bytes_received, elapsed);
        }
        const std::string diagnostic = absl::StrCat("Terminal HTTP error. ",
                                                    HttpFailureDetails(res, response_code, response.bytes_received, elapsed),
                                                    " Body: ", response.body);
        const absl::Status context_status = ContextOverflowStatus(response_code, response.body);
        if (!context_status.ok()) return absl::ResourceExhaustedError(diagnostic);
        return absl::UnavailableError(diagnostic);
      }
    }

    if (IsAborted()) return absl::CancelledError("HTTP request cancelled");
    if (retry_count >= max_retries_) {
      return absl::UnavailableError(absl::StrCat("HTTP request failed after ", retry_count,
                                                 " retries. ", HttpFailureDetails(res, response_code,
                                                                                  response.bytes_received, elapsed)));
    }

    int64_t header_delay = ParseRetryAfter(response_headers);
    if (header_delay == -1) {
      header_delay = ParseXRateLimitReset(response_headers);
    }
    if (header_delay == -1) {
      header_delay = ParseGoogleRetryDelay(response.body);
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
    abort_generation_.fetch_add(1);
  }
  abort_cv_.SignalAll();
}

void HttpClient::CancellableSleep(int64_t wait_ms) {
  absl::MutexLock lock(abort_mutex_);
  auto deadline = absl::Now() + absl::Milliseconds(wait_ms);
  while (!IsAborted() && absl::Now() < deadline) {
    abort_cv_.WaitWithDeadline(&abort_mutex_, deadline);
  }
}

absl::Status HttpClient::ContextOverflowStatus(long response_code, absl::string_view response_body) {
  if (response_code == 413) {
    if (IsDebugHttpEnabled()) {
      LOG(INFO) << "Classified provider response as context overflow: HTTP 413 Payload Too Large.";
    }
    return absl::ResourceExhaustedError("Provider context overflow");
  }
  if (response_code != 400) {
    if (IsDebugHttpEnabled()) {
      LOG(INFO) << "Context-overflow classification skipped for HTTP " << response_code << ".";
    }
    return absl::OkStatus();
  }

  const std::string normalized = absl::AsciiStrToLower(std::string(response_body));
  const char* match_reason = nullptr;
  if (absl::StrContains(normalized, "context length")) {
    match_reason = "context length";
  } else if (absl::StrContains(normalized, "context_length_exceeded")) {
    match_reason = "context_length_exceeded";
  } else if (absl::StrContains(normalized, "maximum context")) {
    match_reason = "maximum context";
  } else if (absl::StrContains(normalized, "maximum number of tokens")) {
    match_reason = "maximum number of tokens";
  } else if (absl::StrContains(normalized, "too many tokens")) {
    match_reason = "too many tokens";
  } else if (absl::StrContains(normalized, "resource_exhausted")) {
    match_reason = "resource_exhausted";
  }

  if (match_reason != nullptr) {
    if (IsDebugHttpEnabled()) {
      LOG(INFO) << "Classified provider response as context overflow: HTTP 400 matched \"" << match_reason
                << "\".";
    }
    return absl::ResourceExhaustedError("Provider context overflow");
  }
  if (IsDebugHttpEnabled()) {
    LOG(INFO) << "Provider response was not classified as context overflow: HTTP 400 had no known match.";
  }
  return absl::OkStatus();
}

bool HttpClient::IsTerminalError(long response_code, const std::string& response_body) {
  // Non-retryable client errors:
  // 400 Bad Request: Request is invalid.
  // 401 Unauthorized: API key missing or invalid.
  // 403 Forbidden: Permission denied or invalid key.
  // 404 Not Found: Model or endpoint not found.
  if (response_code == 400 || response_code == 401 || response_code == 403 || response_code == 404 ||
      response_code == 413) {
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
