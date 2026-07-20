#include "core/http_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
namespace slop {

namespace {

void CloseFd(int fd) {
  if (fd >= 0) close(fd);
}

void DrainHttpRequest(int fd) {
  std::string request;
  char buffer[1024];
  while (request.find("\r\n\r\n") == std::string::npos) {
    const ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
    if (received <= 0) return;
    request.append(buffer, static_cast<size_t>(received));
  }
}

void SendAll(int fd, absl::string_view data) {
  while (!data.empty()) {
    const ssize_t sent = send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (sent <= 0) return;
    data.remove_prefix(static_cast<size_t>(sent));
  }
}

int BindLoopbackServer() {
  const int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) return -1;

  int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  timeval accept_timeout = {};
  accept_timeout.tv_sec = 2;
  setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &accept_timeout, sizeof(accept_timeout));

  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(0);
  if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    CloseFd(listen_fd);
    return -1;
  }
  if (listen(listen_fd, 2) != 0) {
    CloseFd(listen_fd);
    return -1;
  }
  return listen_fd;
}

int BoundPort(int listen_fd) {
  sockaddr_in addr = {};
  socklen_t addr_len = sizeof(addr);
  if (getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0) return -1;
  return ntohs(addr.sin_port);
}

void ServeTwoResponses(int listen_fd, std::atomic<int>* request_count) {
  const std::string responses[] = {
      "HTTP/1.1 429 Too Many Requests\r\nContent-Length: 28\r\nRetry-After: 0\r\nConnection: close\r\n\r\nToo many requests per minute",
      "HTTP/1.1 200 OK\r\nContent-Length: 9\r\nConnection: close\r\n\r\nstream-ok",
  };
  for (const std::string& response : responses) {
    const int client_fd = accept(listen_fd, nullptr, nullptr);
    if (client_fd < 0) break;
    DrainHttpRequest(client_fd);
    SendAll(client_fd, response);
    CloseFd(client_fd);
    request_count->fetch_add(1);
  }
  CloseFd(listen_fd);
}

void ServeOneResponse(int listen_fd, absl::string_view response, std::atomic<int>* request_count) {
  const int client_fd = accept(listen_fd, nullptr, nullptr);
  if (client_fd >= 0) {
    DrainHttpRequest(client_fd);
    SendAll(client_fd, response);
    CloseFd(client_fd);
    request_count->fetch_add(1);
  }
  CloseFd(listen_fd);
}

}  // namespace

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

TEST(HttpClientTest, CaptureHeaderFieldParsesKeyAndValue) {
  absl::flat_hash_map<std::string, std::string> headers;
  HttpClient::CaptureHeaderField("Content-Type: application/json\r\n", &headers);
  HttpClient::CaptureHeaderField("Retry-After: 120\r\n", &headers);

  EXPECT_EQ(headers["content-type"], "application/json");
  EXPECT_EQ(headers["retry-after"], "120");
}

TEST(HttpClientTest, CaptureHeaderFieldIgnoresLinesWithoutColon) {
  absl::flat_hash_map<std::string, std::string> headers;
  HttpClient::CaptureHeaderField("HTTP/1.1 200 OK\r\n", &headers);
  HttpClient::CaptureHeaderField("\r\n", &headers);
  EXPECT_TRUE(headers.empty());
}

TEST(HttpClientTest, CaptureHeaderFieldStripsWhitespace) {
  absl::flat_hash_map<std::string, std::string> headers;
  HttpClient::CaptureHeaderField("  X-Custom :  spaced value  \r\n", &headers);
  ASSERT_EQ(headers.size(), 1);
  EXPECT_EQ(headers.begin()->first, "x-custom");
  EXPECT_EQ(headers.begin()->second, "spaced value");
}

TEST(HttpClientTest, ParseHttpStatusLineValid) {
  long code = 0;
  EXPECT_TRUE(HttpClient::ParseHttpStatusLine("HTTP/1.1 200 OK\r\n", &code));
  EXPECT_EQ(code, 200);

  EXPECT_TRUE(HttpClient::ParseHttpStatusLine("HTTP/2 418 I'm a teapot", &code));
  EXPECT_EQ(code, 418);

  EXPECT_TRUE(HttpClient::ParseHttpStatusLine("  HTTP/1.0 503 Service Unavailable\n", &code));
  EXPECT_EQ(code, 503);
}

TEST(HttpClientTest, ParseHttpStatusLineRejectsMalformed) {
  long code = 42;
  EXPECT_FALSE(HttpClient::ParseHttpStatusLine("not a status line", &code));
  EXPECT_EQ(code, 42) << "out-param must be untouched on failure";

  EXPECT_FALSE(HttpClient::ParseHttpStatusLine("HTTP/1.1\r\n", &code));
  EXPECT_FALSE(HttpClient::ParseHttpStatusLine("HTTP/1.1 abc Bad\r\n", &code));
  EXPECT_FALSE(HttpClient::ParseHttpStatusLine("", &code));
}

TEST(HttpClientTest, PostStreamRetriesTransientErrorBodyBeforeDeliveringChunks) {
  const int listen_fd = BindLoopbackServer();
  ASSERT_GE(listen_fd, 0);
  const int port = BoundPort(listen_fd);
  ASSERT_GT(port, 0);

  std::atomic<int> request_count{0};
  std::thread server([listen_fd, &request_count] { ServeTwoResponses(listen_fd, &request_count); });

  HttpClient client(1, 0);
  std::string delivered;
  auto result = client.PostStream(absl::StrCat("http://127.0.0.1:", port), "{}", {"Content-Type: application/json"},
                                  [&](absl::string_view chunk) {
                                    delivered.append(chunk.data(), chunk.size());
                                    return absl::OkStatus();
                                  });

  server.join();

  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(request_count.load(), 2);
  EXPECT_EQ(*result, "stream-ok");
  EXPECT_EQ(delivered, "stream-ok");
}

TEST(HttpClientTest, PostWithResponseCapturesStatusHeadersAndBody) {
  const int listen_fd = BindLoopbackServer();
  ASSERT_GE(listen_fd, 0);
  const int port = BoundPort(listen_fd);
  ASSERT_GT(port, 0);

  std::atomic<int> request_count{0};
  std::thread server([listen_fd, &request_count] {
    ServeOneResponse(listen_fd,
                     "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nX-Test: ok\r\nContent-Length: 11\r\n"
                     "Connection: close\r\n\r\n{\"ok\":true}",
                     &request_count);
  });

  HttpClient client(0, 0);
  auto response = client.PostWithResponse(absl::StrCat("http://127.0.0.1:", port), "{}", {"Content-Type: application/json"});

  server.join();

  ASSERT_TRUE(response.ok()) << response.status();
  EXPECT_EQ(request_count.load(), 1);
  EXPECT_EQ(response->status_code, 201);
  EXPECT_EQ(response->body, "{\"ok\":true}");
  EXPECT_EQ(response->headers["content-type"], "application/json");
  EXPECT_EQ(response->headers["x-test"], "ok");
}

TEST(HttpClientTest, PostStreamWithResponseCapturesHeadersAndDeliversChunks) {
  const int listen_fd = BindLoopbackServer();
  ASSERT_GE(listen_fd, 0);
  const int port = BoundPort(listen_fd);
  ASSERT_GT(port, 0);

  std::atomic<int> request_count{0};
  std::thread server([listen_fd, &request_count] {
    ServeOneResponse(listen_fd,
                     "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nMcp-Session-Id: abc123\r\n"
                     "Content-Length: 20\r\nConnection: close\r\n\r\ndata: one\n\ndata: two",
                     &request_count);
  });

  HttpClient client(0, 0);
  std::string delivered;
  auto response = client.PostStreamWithResponse(absl::StrCat("http://127.0.0.1:", port), "{}", {},
                                                [&](absl::string_view chunk) {
                                                  delivered.append(chunk.data(), chunk.size());
                                                  return absl::OkStatus();
                                                });

  server.join();

  ASSERT_TRUE(response.ok()) << response.status();
  EXPECT_EQ(request_count.load(), 1);
  EXPECT_EQ(response->status_code, 200);
  EXPECT_EQ(response->body, "data: one\n\ndata: two");
  EXPECT_EQ(delivered, response->body);
  EXPECT_EQ(response->headers["content-type"], "text/event-stream");
  EXPECT_EQ(response->headers["mcp-session-id"], "abc123");
}

TEST(HttpClientTest, PostWithResponsePreservesRetryBehavior) {
  const int listen_fd = BindLoopbackServer();
  ASSERT_GE(listen_fd, 0);
  const int port = BoundPort(listen_fd);
  ASSERT_GT(port, 0);

  std::atomic<int> request_count{0};
  std::thread server([listen_fd, &request_count] { ServeTwoResponses(listen_fd, &request_count); });

  HttpClient client(1, 0);
  auto response = client.PostWithResponse(absl::StrCat("http://127.0.0.1:", port), "{}", {"Content-Type: application/json"});

  server.join();

  ASSERT_TRUE(response.ok()) << response.status();
  EXPECT_EQ(request_count.load(), 2);
  EXPECT_EQ(response->status_code, 200);
  EXPECT_EQ(response->body, "stream-ok");
}

TEST(HttpClientTest, PostWithResponseReturnsAuthErrorMetadata) {
  const int listen_fd = BindLoopbackServer();
  ASSERT_GE(listen_fd, 0);
  const int port = BoundPort(listen_fd);
  ASSERT_GT(port, 0);

  std::atomic<int> request_count{0};
  std::thread server([listen_fd, &request_count] {
    ServeOneResponse(listen_fd,
                     "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Bearer resource_metadata=\"https://auth.example/metadata\"\r\n"
                     "Content-Length: 13\r\nConnection: close\r\n\r\nauth required",
                     &request_count);
  });

  HttpClient client(0, 0);
  auto response = client.PostWithResponse(absl::StrCat("http://127.0.0.1:", port), "{}", {});

  server.join();

  ASSERT_TRUE(response.ok()) << response.status();
  EXPECT_EQ(request_count.load(), 1);
  EXPECT_EQ(response->status_code, 401);
  EXPECT_EQ(response->body, "auth required");
  EXPECT_EQ(response->headers["www-authenticate"], "Bearer resource_metadata=\"https://auth.example/metadata\"");
}

TEST(HttpClientTest, BodyOnlyPostPreservesTerminalAuthErrorBehavior) {
  const int listen_fd = BindLoopbackServer();
  ASSERT_GE(listen_fd, 0);
  const int port = BoundPort(listen_fd);
  ASSERT_GT(port, 0);

  std::atomic<int> request_count{0};
  std::thread server([listen_fd, &request_count] {
    ServeOneResponse(listen_fd,
                     "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Bearer\r\nContent-Length: 13\r\n"
                     "Connection: close\r\n\r\nauth required",
                     &request_count);
  });

  HttpClient client(0, 0);
  auto response = client.Post(absl::StrCat("http://127.0.0.1:", port), "{}", {});

  server.join();

  ASSERT_FALSE(response.ok());
  EXPECT_TRUE(absl::IsUnavailable(response.status()));
  EXPECT_EQ(request_count.load(), 1);
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
