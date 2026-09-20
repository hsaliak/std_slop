#include "mcp/modern_http.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/time/time.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "core/http_client.h"
#include "mcp/modern.h"
#include "mcp/protocol.h"

namespace slop::mcp::v2026_07_28 {
namespace {

class FakeHttpClient : public HttpClient {
 public:
  absl::StatusOr<HttpResponse> PostOnceStreamWithResponse(const std::string& url, const std::string& body,
                                                          const std::vector<std::string>& headers,
                                                          absl::Duration timeout, size_t /*max_response_bytes*/,
                                                          ChunkCallback on_chunk) override {
    ++calls;
    last_url = url;
    last_body = body;
    last_headers = headers;
    last_timeout = timeout;
    if (!status.ok()) return status;
    for (const std::string& chunk : chunks) {
      const absl::Status callback_status = on_chunk(chunk);
      if (!callback_status.ok()) return callback_status;
    }
    return response;
  }

  int calls = 0;
  absl::Status status = absl::OkStatus();
  HttpResponse response;
  std::vector<std::string> chunks;
  std::string last_url;
  std::string last_body;
  std::vector<std::string> last_headers;
  absl::Duration last_timeout;
};

Request MakeRequest(std::string method = "server/discover") {
  Request request;
  request.id = std::string("id-1");
  request.method = std::move(method);
  request.context.client_info.name = "test";
  request.context.client_info.version = "1";
  if (request.method == "tools/call") {
    request.params = {{"name", "tool"}, {"arguments", nlohmann::json::object()}};
  }
  return request;
}

HttpExchangeOptions MakeOptions() {
  HttpExchangeOptions options;
  options.endpoint_url = "https://example.test/mcp";
  return options;
}

bool HasHeader(const std::vector<std::string>& headers, absl::string_view prefix) {
  for (const std::string& header : headers) {
    if (absl::StartsWith(header, prefix)) return true;
  }
  return false;
}

TEST(ModernHttpTest, DecodesChunkedSseNotificationsAndFinalResponse) {
  FakeHttpClient http;
  http.response = {
      200,
      "event: message\ndata: "
      "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\",\"params\":{\"progress\":1}}\n\n"
      "data: {\"jsonrpc\":\"2.0\",\"id\":\"id-1\",\"result\":{\"ok\":true}}\n\n",
      {{"content-type", "text/event-stream"}},
  };
  http.chunks = {http.response.body.substr(0, 17), http.response.body.substr(17)};
  HttpExchange exchange(MakeOptions(), &http);

  auto result = exchange.Execute(MakeRequest());

  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_TRUE(result->response.has_value());
  EXPECT_TRUE(result->response->result.has_value());
  ASSERT_EQ(result->notifications.size(), 1);
  EXPECT_EQ(result->notifications[0].kind, ServerNotificationKind::kProgress);
  EXPECT_EQ(http.calls, 1);
  EXPECT_FALSE(HasHeader(http.last_headers, "Mcp-Session-Id:"));
  EXPECT_FALSE(HasHeader(http.last_headers, "Last-Event-ID:"));
}

TEST(ModernHttpTest, PreservesStructuredErrorsOnNonSuccessStatus) {
  FakeHttpClient http;
  http.response = {
      400,
      R"({"jsonrpc":"2.0","id":"id-1","error":{"code":-32020,"message":"HeaderMismatch","data":{"field":"Mcp-Name"}}})",
      {{"content-type", "application/json"}},
  };
  HttpExchange exchange(MakeOptions(), &http);

  auto result = exchange.Execute(MakeRequest("tools/call"));

  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_TRUE(result->failure.has_value());
  EXPECT_EQ(result->failure->http_status, 400);
  EXPECT_EQ(result->failure->json_rpc_code, -32020);
  EXPECT_EQ(result->failure->execution, ExecutionCertainty::kNotExecuted);
  EXPECT_EQ(result->failure->json_rpc_data["field"], "Mcp-Name");
  EXPECT_EQ(http.calls, 1);
}

TEST(ModernHttpTest, PreservesStructuredErrorWithNullIdOnNonSuccessStatus) {
  FakeHttpClient http;
  http.response = {
      400,
      R"({"jsonrpc":"2.0","id":null,"error":{"code":-32000,"message":"Unsupported protocol version"}})",
      {{"content-type", "application/json"}},
  };
  HttpExchange exchange(MakeOptions(), &http);

  auto result = exchange.Execute(MakeRequest());

  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_TRUE(result->failure.has_value());
  EXPECT_EQ(result->failure->http_status, 400);
  EXPECT_EQ(result->failure->json_rpc_code, -32000);
}

TEST(ModernHttpTest, MarksLostResponseAsAmbiguousWithoutReplay) {
  FakeHttpClient http;
  http.status = absl::DeadlineExceededError("response lost");
  HttpExchange exchange(MakeOptions(), &http);

  auto result = exchange.Execute(MakeRequest("tools/call"));

  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_TRUE(result->failure.has_value());
  EXPECT_EQ(result->failure->execution, ExecutionCertainty::kMayHaveExecuted);
  EXPECT_EQ(http.calls, 1);
}

TEST(ModernHttpTest, EnforcesResponseAndMessageLimits) {
  FakeHttpClient bytes_http;
  bytes_http.response = {200, "12345", {{"content-type", "application/json"}}};
  bytes_http.chunks = {"123", "45"};
  HttpExchangeOptions byte_options = MakeOptions();
  byte_options.max_response_bytes = 4;
  HttpExchange byte_exchange(byte_options, &bytes_http);
  auto bytes = byte_exchange.Execute(MakeRequest());
  ASSERT_TRUE(bytes.ok());
  ASSERT_TRUE(bytes->failure.has_value());
  EXPECT_NE(bytes->failure->message.find("byte limit"), std::string::npos);

  FakeHttpClient event_http;
  event_http.response = {
      200,
      "data: {\"jsonrpc\":\"2.0\",\"method\":\"one\"}\n\n"
      "data: {\"jsonrpc\":\"2.0\",\"id\":\"id-1\",\"result\":{}}\n\n",
      {{"content-type", "text/event-stream"}},
  };
  event_http.chunks = {event_http.response.body};
  HttpExchangeOptions event_options = MakeOptions();
  event_options.max_messages = 1;
  HttpExchange event_exchange(event_options, &event_http);
  EXPECT_EQ(event_exchange.Execute(MakeRequest()).status().code(), absl::StatusCode::kResourceExhausted);
}

TEST(ModernHttpTest, RejectsReservedHeadersBeforeNetwork) {
  FakeHttpClient http;
  HttpExchangeOptions options = MakeOptions();
  options.extra_headers["Authorization"] = "secret";
  HttpExchange exchange(options, &http);

  auto result = exchange.Execute(MakeRequest());

  EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(http.calls, 0);
}

TEST(ModernHttpTest, RejectsMismatchedAndTruncatedResponses) {
  FakeHttpClient mismatch_http;
  mismatch_http.response = {
      200,
      R"({"jsonrpc":"2.0","id":"other","result":{}})",
      {{"content-type", "application/json"}},
  };
  mismatch_http.chunks = {mismatch_http.response.body};
  HttpExchange mismatch_exchange(MakeOptions(), &mismatch_http);
  EXPECT_EQ(mismatch_exchange.Execute(MakeRequest()).status().code(), absl::StatusCode::kInvalidArgument);

  FakeHttpClient truncated_http;
  truncated_http.response = {
      200,
      "data: {\"jsonrpc\":\"2.0\",\"id\":\"id-1\"",
      {{"content-type", "text/event-stream"}},
  };
  truncated_http.chunks = {truncated_http.response.body};
  HttpExchange truncated_exchange(MakeOptions(), &truncated_http);
  EXPECT_EQ(truncated_exchange.Execute(MakeRequest()).status().code(), absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace slop::mcp::v2026_07_28
