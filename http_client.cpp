#include "http_client.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <iostream>
#include <thread>

#include "absl/status/status.h"
#include "absl/strings/match.h"
namespace slop {

namespace {
struct CurlDeleter {
  void operator()(CURL* curl) const { if (curl) curl_easy_cleanup(curl); }
};
struct SlistDeleter {
  void operator()(struct curl_slist* list) const { if (list) curl_slist_free_all(list); }
};
} // namespace

HttpClient::HttpClient() {
  curl_global_init(CURL_GLOBAL_ALL);
}

HttpClient::~HttpClient() {
  curl_global_cleanup();
}

size_t HttpClient::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
  static_cast<std::string*>(userp)->append(static_cast<const char*>(contents), size * nmemb);
  return size * nmemb;
}

int HttpClient::ProgressCallback(void* clientp, [[maybe_unused]]  curl_off_t dltotal, [[maybe_unused]]  curl_off_t dlnow, [[maybe_unused]]  curl_off_t ultotal, [[maybe_unused]]  curl_off_t ulnow) {
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

    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    int oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);

    int ch = getchar();

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);

    if (ch == 27) {
        std::cout << "\n[Cancelled by user]" << std::endl;
        client->Abort();
        return 1;
    }

    return 0;
}

absl::StatusOr<std::string> HttpClient::Post(const std::string& url,
                                             const std::string& body,
                                             const std::vector<std::string>& headers) {
  return ExecuteWithRetry(url, "POST", body, headers);
}

absl::StatusOr<std::string> HttpClient::Get(const std::string& url,
                                            const std::vector<std::string>& headers) {
  return ExecuteWithRetry(url, "GET", "", headers);
}

absl::StatusOr<std::string> HttpClient::ExecuteWithRetry(const std::string& url,
                                                         const std::string& method,
                                                         const std::string& body,
                                                         const std::vector<std::string>& headers) {
  ResetAbort();

  int max_retries = 3;
  int retry_count = 0;
  int64_t backoff_ms = 1000;

  std::unique_ptr<CURL, CurlDeleter> curl(curl_easy_init());
  if (!curl) return absl::InternalError("Failed to initialize CURL");

  struct curl_slist* raw_chunk = nullptr;
  for (const auto& header : headers) {
    raw_chunk = curl_slist_append(raw_chunk, header.c_str());
  }
  std::unique_ptr<struct curl_slist, SlistDeleter> chunk(raw_chunk);

  while (true) {
    std::string response_string;

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    if (method == "POST") {
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
    } else {
        curl_easy_setopt(curl.get(), CURLOPT_HTTPGET, 1L);
    }
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, chunk.get());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, HttpClient::WriteCallback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response_string);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, HttpClient::ProgressCallback);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 60L);

    CURLcode res = curl_easy_perform(curl.get());

    long response_code = 0;  // NOLINT(runtime/int)
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response_code);

    if (res != CURLE_OK) {
      if (this->abort_requested_) return absl::CancelledError("Request cancelled by user");

      if (retry_count < max_retries) {
          std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
          retry_count++;
          backoff_ms *= 2;
          continue;
      }
      return absl::InternalError("CURL error: " + std::string(curl_easy_strerror(res)));
    }

    if (response_code >= 200 && response_code < 300) {
      return response_string;
    }

    if (response_code >= 500 || response_code == 429) {
      if (retry_count < max_retries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
        retry_count++;
        backoff_ms *= 2;
        continue;
      }
    }

    return absl::Status(static_cast<absl::StatusCode>(response_code),
                       "HTTP error " + std::to_string(response_code) + ": " + response_string);
  }
}

} // namespace slop
