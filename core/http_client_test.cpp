#include "core/http_client.h"

#include <cstdlib>
#include <thread>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/match.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
namespace slop {

TEST(HttpClientTest, PostInit) {
  HttpClient client(0, 0);
  // Basic test to ensure it doesn't crash
}

TEST(HttpClientTest, GetError) {
  HttpClient client(0, 0);
  // Should fail on a non-existent local port or invalid URL
  auto res = client.Get("http://localhost:1", {});
  EXPECT_FALSE(res.ok());
}

TEST(HttpClientTest, HttpsSupport) {
  // Check if libcurl has SSL support enabled
  curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
  ASSERT_NE(info, nullptr);
  EXPECT_TRUE(info->features & CURL_VERSION_SSL) << "libcurl was built without SSL support";

  HttpClient client(0, 0);
  auto res = client.Get("https://www.google.com", {});
  // If protocol is unsupported, it will return an InternalError with "Unsupported protocol"
  if (!res.ok()) {
    EXPECT_FALSE(absl::StrContains(res.status().message(), "Unsupported protocol"))
        << "HTTPS protocol is not supported in the current libcurl build: " << res.status().message();
  }
}

TEST(HttpClientTest, PostBasic) {
  HttpClient client(0, 0);
  // We don't have a mock server, but we can at least check if it handles
  // a non-existent endpoint correctly without crashing.
  auto res = client.Post("http://localhost:1", "{\"test\":true}", {"Content-Type: application/json"});
  EXPECT_FALSE(res.ok());
}

TEST(HttpClientTest, ParseRetryAfterSeconds) {
  HttpClient client(0, 0);
  absl::flat_hash_map<std::string, std::string> headers = {{"retry-after", "30"}};
  EXPECT_EQ(client.ParseRetryAfter(headers), 30000);
}

TEST(HttpClientTest, ParseRetryAfterDate) {
  HttpClient client(0, 0);
  // Use a date in the future
  absl::Time future = absl::Now() + absl::Seconds(60);
  std::string date_str = absl::FormatTime("%a, %d %b %Y %H:%M:%S GMT", future, absl::UTCTimeZone());
  absl::flat_hash_map<std::string, std::string> headers = {{"retry-after", date_str}};

  int64_t delay = client.ParseRetryAfter(headers);
  // Should be around 60000ms, allow some slack for execution time
  EXPECT_GT(delay, 55000);
  EXPECT_LE(delay, 65000);
}

TEST(HttpClientTest, ParseRetryAfterMissing) {
  HttpClient client(0, 0);
  absl::flat_hash_map<std::string, std::string> headers = {{"content-type", "application/json"}};
  EXPECT_EQ(client.ParseRetryAfter(headers), -1);
}

TEST(HttpClientTest, HeaderCallback) {
  absl::flat_hash_map<std::string, std::string> headers;
  std::string h1 = "Content-Type: application/json\r\n";
  HttpClient::HeaderCallback(const_cast<char*>(h1.data()), 1, h1.size(), &headers);

  std::string h2 = "Retry-After: 120\r\n";
  HttpClient::HeaderCallback(const_cast<char*>(h2.data()), 1, h2.size(), &headers);

  EXPECT_EQ(headers["content-type"], "application/json");
  EXPECT_EQ(headers["retry-after"], "120");
}

TEST(HttpClientTest, ParseXRateLimitResetTimestamp) {
  HttpClient client(0, 0);
  int64_t future_ts = absl::ToUnixSeconds(absl::Now()) + 60;
  absl::flat_hash_map<std::string, std::string> headers = {{"x-ratelimit-reset", std::to_string(future_ts)}};
  int64_t delay = client.ParseXRateLimitReset(headers);
  EXPECT_GT(delay, 55000);
  EXPECT_LE(delay, 65000);
}

TEST(HttpClientTest, ParseXRateLimitResetRelative) {
  HttpClient client(0, 0);
  absl::flat_hash_map<std::string, std::string> headers = {{"x-ratelimit-reset", "5.5"}};
  EXPECT_EQ(client.ParseXRateLimitReset(headers), 5500);
}

TEST(HttpClientTest, ParseGoogleRetryInfo) {
  HttpClient client(0, 0);
  std::string body = R"({
  "error": {
    "details": [
      {
        "@type": "type.googleapis.com/google.rpc.RetryInfo",
        "retryDelay": "0.421239755s"
      }
    ]
  }
})";
  EXPECT_EQ(client.ParseGoogleRetryDelay(body), 421);
}

TEST(HttpClientTest, ParseGoogleErrorInfoDelay) {
  HttpClient client(0, 0);
  std::string body = R"({
  "error": {
    "details": [
      {
        "@type": "type.googleapis.com/google.rpc.ErrorInfo",
        "reason": "RATE_LIMIT_EXCEEDED",
        "domain": "cloudcode-pa.googleapis.com",
        "metadata": {
          "quotaResetDelay": "2.923127754s"
        }
      }
    ]
  }
})";
  EXPECT_EQ(client.ParseGoogleRetryDelay(body), 2923);
}

TEST(HttpClientTest, ParseGoogleErrorMessageDelay) {
  HttpClient client(0, 0);
  std::string body = R"({
  "error": {
    "code": 429,
    "message": "You have exhausted your capacity on this model. Your quota will reset after 19s.",
    "status": "RESOURCE_EXHAUSTED"
  }
})";
  EXPECT_EQ(client.ParseGoogleRetryDelay(body), 19000);
}

TEST(HttpClientTest, ParseGoogleRetryDelayRobustness) {
  HttpClient client(0, 0);
  // Test with non-object error
  EXPECT_EQ(client.ParseGoogleRetryDelay(R"({"error": "not an object"})"), -1);
  // Test with non-array details
  EXPECT_EQ(client.ParseGoogleRetryDelay(R"({"error": {"details": "not an array"}})"), -1);
  // Test with missing metadata
  EXPECT_EQ(client.ParseGoogleRetryDelay(
                R"({"error": {"details": [{"@type": "type.googleapis.com/google.rpc.ErrorInfo"}]}})"),
            -1);
  // Test with malformed duration
  EXPECT_EQ(client.ParseGoogleRetryDelay(R"({"error": {"message": "Your quota will reset after infinity."}})"), -1);
}

TEST(HttpClientTest, IsTerminalErrorTest) {
  HttpClient client(0, 0);

  // Case 1: Not a terminal code (e.g. 500)
  EXPECT_FALSE(client.IsTerminalError(500, "{\"error\": \"QUOTA_EXHAUSTED\"}"));

  // Case 2: 429 with QUOTA_EXHAUSTED
  EXPECT_TRUE(client.IsTerminalError(429, "{\"error\": \"QUOTA_EXHAUSTED\"}"));

  // Case 3: 403 with QUOTA_EXHAUSTED
  EXPECT_TRUE(client.IsTerminalError(403, "QUOTA_EXHAUSTED in a raw string"));

  // Case 4: 429 without QUOTA_EXHAUSTED (transient)
  EXPECT_FALSE(client.IsTerminalError(429, "Too many requests per minute"));

  // Case 5: 403 without QUOTA_EXHAUSTED
  EXPECT_FALSE(client.IsTerminalError(403, "Access denied"));
}

}  // namespace slop
