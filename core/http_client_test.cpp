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

TEST(HttpClientTest, AbortDoesNotPoisonNextRequest) {
  HttpClient client(0, 0);

  client.Abort();
  EXPECT_TRUE(client.IsAborted());

  auto res = client.Get("http://localhost:1", {});
  EXPECT_FALSE(res.ok());
  EXPECT_TRUE(absl::IsCancelled(res.status()));
  EXPECT_FALSE(client.IsAborted());
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
  // If connection fails, it will return Unavailable.
  // Either way, it shouldn't crash and we check if we can at least reach a major HTTPS site.
  if (!res.ok()) {
    EXPECT_TRUE(absl::IsUnavailable(res.status()) || absl::IsInternal(res.status()));
  }
}

TEST(HttpClientTest, PostBasic) {
  HttpClient client(0, 0);
  // This will likely fail but we check the retry logic doesn't loop forever
  auto res = client.Post("http://localhost:1", "{}", {});
  EXPECT_FALSE(res.ok());
}

TEST(HttpClientTest, PostStreamPropagatesTransportFailure) {
  HttpClient client(0, 0);
  int chunks = 0;
  auto response = client.PostStream("http://localhost:1", "{}", {}, [&chunks](absl::string_view) {
    ++chunks;
    return absl::OkStatus();
  });
  EXPECT_FALSE(response.ok());
  EXPECT_EQ(chunks, 0);
}

TEST(HttpClientTest, ParseRetryAfterSeconds) {
  HttpClient client(0, 0);
  absl::flat_hash_map<std::string, std::string> headers = {{"retry-after", "120"}};
  EXPECT_EQ(client.ParseRetryAfter(headers), 120000);
}

TEST(HttpClientTest, ParseRetryAfterDate) {
  HttpClient client(0, 0);
  // Set date to 60 seconds in the future
  time_t now = time(nullptr);
  struct tm* gmt = gmtime(&now);
  gmt->tm_sec += 60;
  mktime(gmt);
  char buf[64];
  strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", gmt);

  absl::flat_hash_map<std::string, std::string> headers = {{"retry-after", buf}};
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

  // Case 1: Not a terminal code (e.g. 500) - Should retry
  EXPECT_FALSE(client.IsTerminalError(500, "Internal Server Error"));

  // Case 2: 400, 401, 403, 404 - Should NOT retry
  EXPECT_TRUE(client.IsTerminalError(400, "Bad Request"));
  EXPECT_TRUE(client.IsTerminalError(401, "Unauthorized"));
  EXPECT_TRUE(client.IsTerminalError(403, "Forbidden"));
  EXPECT_TRUE(client.IsTerminalError(404, "Not Found"));
  // Context overflow uses terminal 400/413 responses and is later normalized
  // to ResourceExhausted by the request path.
  EXPECT_TRUE(client.IsTerminalError(400, R"({"error":{"code":"context_length_exceeded"}})"));
  EXPECT_TRUE(client.IsTerminalError(413, R"({"error":{"status":"RESOURCE_EXHAUSTED"}})"));
  EXPECT_TRUE(client.IsTerminalError(413, "Payload Too Large"));
  EXPECT_EQ(HttpClient::ContextOverflowStatus(413, "Payload Too Large").code(),
            absl::StatusCode::kResourceExhausted);
  EXPECT_EQ(HttpClient::ContextOverflowStatus(400, R"({"error":{"code":"context_length_exceeded"}})").code(),
            absl::StatusCode::kResourceExhausted);
  EXPECT_TRUE(HttpClient::ContextOverflowStatus(400, "invalid request").ok());

  // Case 3: 429 with QUOTA_EXHAUSTED
  EXPECT_TRUE(client.IsTerminalError(429, "{\"error\": \"QUOTA_EXHAUSTED\"}"));

  // Case 4: 429 without QUOTA_EXHAUSTED (transient) - Should retry
  EXPECT_FALSE(client.IsTerminalError(429, "Too many requests per minute"));
}

}  // namespace slop
