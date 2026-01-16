#include "http_client.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include <iostream>
#include <thread>
#include <chrono>

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

namespace slop {

HttpClient::HttpClient() {
  curl_global_init(CURL_GLOBAL_ALL);
}

HttpClient::~HttpClient() {
  curl_global_cleanup();
}

size_t HttpClient::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
  ((std::string*)userp)->append((char*)contents, size * nmemb);
  return size * nmemb;
}

int HttpClient::ProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    HttpClient* client = static_cast<HttpClient*>(clientp);
    if (client->abort_requested_) {
        return 1; // Non-zero returns abort the transfer
    }

    // Check for Esc key (ASCII 27)
    struct termios oldt, newt;
    int ch;
    int oldf;

    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);

    ch = getchar();

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);

    if (ch == 27) { // Escape key
        std::cout << "\n[Cancelled by user]" << std::endl;
        client->Abort();
        return 1;
    }

    return 0;
}

struct CurlDeleter {
  void operator()(CURL* curl) const { if (curl) curl_easy_cleanup(curl); }
};
struct SlistDeleter {
  void operator()(struct curl_slist* list) const { if (list) curl_slist_free_all(list); }
};

absl::StatusOr<std::string> HttpClient::Post(const std::string& url,
                                             const std::string& body,
                                             const std::vector<std::string>& headers) {
  ResetAbort();
  
  int max_retries = 5;
  int retry_count = 0;
  long backoff_ms = 1000;

  while (true) {
    std::unique_ptr<CURL, CurlDeleter> curl(curl_easy_init());
    if (!curl) {
      return absl::InternalError("Failed to initialize CURL");
    }

    std::string response_string;
    struct curl_slist* raw_chunk = nullptr;
    for (const auto& header : headers) {
      raw_chunk = curl_slist_append(raw_chunk, header.c_str());
    }
    std::unique_ptr<struct curl_slist, SlistDeleter> chunk(raw_chunk);

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, chunk.get());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, HttpClient::WriteCallback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response_string);

    // Progress callback for cancellation
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, HttpClient::ProgressCallback);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);

    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 60L);

    CURLcode res = curl_easy_perform(curl.get());
    
    long response_code = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response_code);

    if (res != CURLE_OK) {
      if (this->abort_requested_) {
          return absl::CancelledError("Request cancelled by user");
      }
      return absl::InternalError("CURL request failed: " + std::string(curl_easy_strerror(res)));
    }

    if (response_code == 429 || 
        absl::StrContains(response_string, "RESOURCE_EXHAUSTED") ||
        absl::StrContains(response_string, "REQUEST_EXCEEDED") ||
        absl::StrContains(response_string, "Quota exceeded")) {
      return absl::ResourceExhaustedError("Rate limit exceeded: " + response_string);
    }

    if (response_code >= 400) {
      return absl::InternalError("HTTP error " + std::to_string(response_code) + ": " + response_string);
    }

    return response_string;
  }
}

absl::StatusOr<std::string> HttpClient::Get(const std::string& url,
                                            const std::vector<std::string>& headers) {
  int max_retries = 3;
  int retry_count = 0;
  long backoff_ms = 1000;

  while (true) {
    std::unique_ptr<CURL, CurlDeleter> curl(curl_easy_init());
    if (!curl) {
      return absl::InternalError("Failed to initialize CURL");
    }

    std::string response_string;
    struct curl_slist* raw_chunk = nullptr;
    for (const auto& header : headers) {
      raw_chunk = curl_slist_append(raw_chunk, header.c_str());
    }
    std::unique_ptr<struct curl_slist, SlistDeleter> chunk(raw_chunk);

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, chunk.get());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, HttpClient::WriteCallback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response_string);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 60L);

    CURLcode res = curl_easy_perform(curl.get());
    
    long response_code = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response_code);

    if (res != CURLE_OK) {
      return absl::InternalError("CURL request failed: " + std::string(curl_easy_strerror(res)));
    }

    if (response_code == 429 || (response_code >= 500 && response_code <= 599)) {
      if (retry_count < max_retries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
        retry_count++;
        backoff_ms *= 2;
        continue;
      }
    }

    if (response_code >= 400) {
      return absl::InternalError("HTTP error " + std::to_string(response_code) + ": " + response_string);
    }

    return response_string;
  }
}

}  // namespace slop
