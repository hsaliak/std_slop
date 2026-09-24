#include "mcp/client/streamable_http_transport.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "core/http_client.h"
#include "core/json_utils.h"

namespace slop::mcp::v2025_11_25 {
namespace {

class FakeHttpClient : public HttpClient {
 public:
  absl::StatusOr<HttpResponse> PostStreamWithResponse(const std::string& url, const std::string& body,
                                                      const std::vector<std::string>& headers,
                                                      ChunkCallback on_chunk) override {
    last_url = url;
    last_body = body;
    last_headers = headers;
    if (!status.ok()) return status;
    if (on_chunk && !response.body.empty()) {
      absl::Status callback_status = on_chunk(response.body);
      if (!callback_status.ok()) return callback_status;
    }
    return response;
  }

  absl::StatusOr<HttpResponse> PostOnceStreamWithResponse(const std::string& url, const std::string& body,
                                                          const std::vector<std::string>& headers, absl::Duration,
                                                          size_t, ChunkCallback on_chunk) override {
    return PostStreamWithResponse(url, body, headers, std::move(on_chunk));
  }

  HttpResponse response;
  absl::Status status = absl::OkStatus();
  std::string last_url;
  std::string last_body;
  std::vector<std::string> last_headers;
};

bool HasHeader(const std::vector<std::string>& headers, const std::string& expected) {
  return std::find(headers.begin(), headers.end(), expected) != headers.end();
}

TEST(StreamableHttpTransportTest, SendsRequiredHeadersAndParsesJsonResponse) {
  FakeHttpClient http;
  http.response = {200,
                   R"({"jsonrpc":"2.0","id":1,"result":{"ok":true}})",
                   {{"content-type", "application/json"}, {"mcp-session-id", "sess-1"}}};
  StreamableHttpConfig config;
  config.endpoint_url = "https://example.com/mcp";
  config.bearer_token = "token";
  config.extra_headers["X-Extra"] = "value";
  StreamableHttpTransport transport(config, &http);
  transport.SetProtocolVersion("2025-11-25");

  ASSERT_TRUE(transport.Start().ok());
  ASSERT_TRUE(transport.Send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}}).ok());
  auto received = transport.Receive(absl::Seconds(1));

  ASSERT_TRUE(received.ok()) << received.status();
  EXPECT_EQ(http.last_url, "https://example.com/mcp");
  EXPECT_EQ(json_get_or(*received, "jsonrpc", std::string{}), "2.0");
  EXPECT_TRUE(HasHeader(http.last_headers, "Content-Type: application/json"));
  EXPECT_TRUE(HasHeader(http.last_headers, "Accept: application/json, text/event-stream"));
  EXPECT_TRUE(HasHeader(http.last_headers, "MCP-Protocol-Version: 2025-11-25"));
  EXPECT_TRUE(HasHeader(http.last_headers, "Authorization: Bearer token"));
  EXPECT_TRUE(HasHeader(http.last_headers, "X-Extra: value"));
  EXPECT_EQ(transport.session_id(), "sess-1");
}

TEST(StreamableHttpTransportTest, RejectsInvalidAndReservedExtraHeaders) {
  for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{
           {"Authorization", "secret"}, {"Bad Header", "value"}, {"X-Test", "value\r\ninjected: true"}}) {
    FakeHttpClient http;
    StreamableHttpConfig config;
    config.endpoint_url = "https://example.com/mcp";
    config.extra_headers[name] = value;
    StreamableHttpTransport transport(config, &http);
    ASSERT_TRUE(transport.Start().ok());
    EXPECT_EQ(transport.Send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}}).code(),
              absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(http.last_url.empty());
  }
}

TEST(StreamableHttpTransportTest, ParsesSseMessages) {
  FakeHttpClient http;
  http.response = {200,
                   "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n\n"
                   "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"}\n\n",
                   {{"content-type", "text/event-stream"}}};
  StreamableHttpConfig config;
  config.endpoint_url = "https://example.com/mcp";
  StreamableHttpTransport transport(config, &http);

  ASSERT_TRUE(transport.Start().ok());
  ASSERT_TRUE(transport.Send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}}).ok());

  auto first = transport.Receive(absl::Seconds(1));
  auto second = transport.Receive(absl::Seconds(1));
  ASSERT_TRUE(first.ok()) << first.status();
  ASSERT_TRUE(second.ok()) << second.status();
  EXPECT_EQ(json_get_or(*first, "id", 0), 1);
  EXPECT_EQ(json_get_or(*second, "method", std::string{}), "notifications/progress");
}

TEST(StreamableHttpTransportTest, MapsUnauthorized) {
  FakeHttpClient http;
  http.response = {401, "auth required", {{"content-type", "text/plain"}}};
  StreamableHttpConfig config;
  config.endpoint_url = "https://example.com/mcp";
  StreamableHttpTransport transport(config, &http);

  ASSERT_TRUE(transport.Start().ok());
  auto status = transport.Send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}});

  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.code(), absl::StatusCode::kUnauthenticated);
}

TEST(StreamableHttpTransportTest, AcceptsEmptyAcceptedResponseWithoutQueuedMessage) {
  FakeHttpClient http;
  http.response = {202, "", {}};
  StreamableHttpConfig config;
  config.endpoint_url = "https://example.com/mcp";
  StreamableHttpTransport transport(config, &http);

  ASSERT_TRUE(transport.Start().ok());
  ASSERT_TRUE(transport.Send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).ok());
  auto received = transport.Receive(absl::Seconds(1));

  ASSERT_FALSE(received.ok());
  EXPECT_EQ(received.status().code(), absl::StatusCode::kUnavailable);
}

TEST(StreamableHttpTransportTest, RejectsUnsupportedMediaType) {
  FakeHttpClient http;
  http.response = {200, "plain", {{"content-type", "text/plain"}}};
  StreamableHttpConfig config;
  config.endpoint_url = "https://example.com/mcp";
  StreamableHttpTransport transport(config, &http);

  ASSERT_TRUE(transport.Start().ok());
  auto status = transport.Send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}});

  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
}

TEST(StreamableHttpTransportTest, RejectsNullHttpClient) {
  StreamableHttpConfig config;
  config.endpoint_url = "https://example.com/mcp";
  StreamableHttpTransport transport(config, nullptr);

  auto status = transport.Start();

  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace slop::mcp::v2025_11_25
