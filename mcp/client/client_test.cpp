#include "mcp/client/client.h"

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "core/http_client.h"
#include "mcp/protocol.h"

namespace slop::mcp {
namespace {

using v2025_11_25::InitializeOptions;

class FakeHttpClient : public HttpClient {
 public:
  absl::StatusOr<HttpResponse> PostStreamWithResponse(const std::string& url, const std::string& body,
                                                      const std::vector<std::string>& headers,
                                                      ChunkCallback on_chunk) override {
    last_url = url;
    bodies.push_back(body);
    last_headers = headers;
    if (!status.ok()) return status;
    if (responses.empty()) return absl::UnavailableError("no response queued");
    HttpResponse response = responses.front();
    responses.erase(responses.begin());
    if (on_chunk && !response.body.empty()) {
      absl::Status callback_status = on_chunk(response.body);
      if (!callback_status.ok()) return callback_status;
    }
    return response;
  }

  absl::StatusOr<HttpResponse> PostOnceStreamWithResponse(const std::string& url, const std::string& body,
                                                          const std::vector<std::string>& headers, absl::Duration,
                                                          size_t, ChunkCallback on_chunk) override {
    bool is_modern = false;
    for (const std::string& header : headers) {
      is_modern = is_modern || header.find("2026-07-28") != std::string::npos;
    }
    if (is_modern) ++modern_calls;
    last_url = url;
    bodies.push_back(body);
    last_headers = headers;
    if (!status.ok()) return status;
    if (responses.empty()) return absl::UnavailableError("no response queued");
    HttpResponse response = responses.front();
    responses.erase(responses.begin());
    if (on_chunk && !response.body.empty()) {
      const absl::Status callback_status = on_chunk(response.body);
      if (!callback_status.ok()) return callback_status;
    }
    return response;
  }

  int modern_calls = 0;
  absl::Status status = absl::OkStatus();
  std::vector<HttpResponse> responses;
  std::string last_url;
  std::vector<std::string> bodies;
  std::vector<std::string> last_headers;
};

InitializeOptions MakeOptions() {
  InitializeOptions options;
  options.client_info.name = "client-test";
  options.client_info.version = "1.0";
  return options;
}

HttpResponse InitializeResponse() {
  return {200,
          R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-11-25","capabilities":{"tools":{}}}})",
          {{"content-type", "application/json"}}};
}

ClientOptions MakeClientOptions(SelectionPolicy policy = SelectionPolicy::kPreferLatest) {
  ClientOptions options;
  options.selection = policy;
  options.client_info.name = "client-test";
  options.client_info.version = "1.0";
  return options;
}

HttpResponse JsonResponse(std::string body, long status = 200) {
  return {status, std::move(body), {{"content-type", "application/json"}}};
}

TEST(McpClientTest, ConnectClassicStreamableHttpRejectsNullHttpClient) {
  StreamableHttpConfig config;
  config.endpoint_url = "https://example.com/mcp";

  auto session = ConnectClassicStreamableHttp(config, MakeOptions(), nullptr);

  ASSERT_FALSE(session.ok());
  EXPECT_EQ(session.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(McpClientTest, ConnectClassicStreamableHttpRejectsEmptyEndpoint) {
  FakeHttpClient http;
  StreamableHttpConfig config;
  http.responses.push_back(InitializeResponse());

  auto session = ConnectClassicStreamableHttp(config, MakeOptions(), &http);

  ASSERT_FALSE(session.ok());
  EXPECT_EQ(session.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(McpClientTest, SelectsModernAndExecutesTools) {
  FakeHttpClient http;
  http.responses.push_back(JsonResponse(
      R"({"jsonrpc":"2.0","id":"modern-1","result":{"supportedVersions":["2026-07-28"],"capabilities":{"tools":{}}}})"));
  http.responses.push_back(JsonResponse(
      R"({"jsonrpc":"2.0","id":"modern-2","result":{"tools":[{"name":"echo","inputSchema":{"type":"object","required":["text"],"properties":{"text":{"type":"string"}}}}]}})"));
  http.responses.push_back(JsonResponse(
      R"({"jsonrpc":"2.0","id":"modern-3","result":{"resultType":"input_required","requestState":{"step":1},"content":[]}})"));
  http.responses.push_back(JsonResponse(
      R"({"jsonrpc":"2.0","id":"modern-4","result":{"resultType":"complete","content":[{"type":"text","text":"ok"}],"structuredContent":[1,2]}})"));
  StreamableHttpConfig config;
  config.endpoint_url = "https://example.com/mcp";

  auto client = ConnectMcp(config, MakeClientOptions(), &http);
  ASSERT_TRUE(client.ok()) << client.status();
  EXPECT_EQ((*client)->revision(), ProtocolRevision::k2026_07_28);
  auto tools = (*client)->ListTools();
  ASSERT_TRUE(tools.ok()) << tools.status();
  ASSERT_EQ(tools->size(), 1);
  auto result = (*client)->CallTool("echo", {{"text", "hello"}});
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(result->kind, ToolResultKind::kInputRequired);
  ASSERT_TRUE(result->request_state.has_value());
  auto continued = (*client)->ContinueToolCall("echo", {{"text", "hello"}}, *result->request_state);
  ASSERT_TRUE(continued.ok()) << continued.status();
  EXPECT_EQ(continued->kind, ToolResultKind::kComplete);
  ASSERT_TRUE(continued->structured_content.has_value());
  EXPECT_TRUE(continued->structured_content->is_array());
  EXPECT_EQ(http.modern_calls, 4);
  EXPECT_NE(http.bodies.back().find("requestState"), std::string::npos);
}

TEST(McpClientTest, AggregatesPagesAndRejectsCursorCycles) {
  StreamableHttpConfig config;
  config.endpoint_url = "https://example.com/mcp";
  FakeHttpClient paged;
  paged.responses.push_back(JsonResponse(
      R"({"jsonrpc":"2.0","id":"modern-1","result":{"supportedVersions":["2026-07-28"],"capabilities":{}}})"));
  paged.responses.push_back(JsonResponse(
      R"({"jsonrpc":"2.0","id":"modern-2","result":{"tools":[{"name":"one","inputSchema":{"type":"object"}}],"nextCursor":"next"}})"));
  paged.responses.push_back(JsonResponse(
      R"({"jsonrpc":"2.0","id":"modern-3","result":{"tools":[{"name":"two","inputSchema":{"type":"object"}}]}})"));
  auto client = ConnectMcp(config, MakeClientOptions(), &paged);
  ASSERT_TRUE(client.ok()) << client.status();
  auto tools = (*client)->ListTools();
  ASSERT_TRUE(tools.ok()) << tools.status();
  ASSERT_EQ(tools->size(), 2);
  EXPECT_EQ((*tools)[0].name, "one");
  EXPECT_EQ((*tools)[1].name, "two");

  FakeHttpClient cycle;
  cycle.responses.push_back(JsonResponse(
      R"({"jsonrpc":"2.0","id":"modern-1","result":{"supportedVersions":["2026-07-28"],"capabilities":{}}})"));
  cycle.responses.push_back(
      JsonResponse(R"({"jsonrpc":"2.0","id":"modern-2","result":{"tools":[],"nextCursor":"same"}})"));
  cycle.responses.push_back(
      JsonResponse(R"({"jsonrpc":"2.0","id":"modern-3","result":{"tools":[],"nextCursor":"same"}})"));
  auto cycle_client = ConnectMcp(config, MakeClientOptions(), &cycle);
  ASSERT_TRUE(cycle_client.ok()) << cycle_client.status();
  EXPECT_EQ((*cycle_client)->ListTools().status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(McpClientTest, PreferLatestFallsBackOnUnrecognizedDiscovery400) {
  FakeHttpClient http;
  http.responses.push_back({400, "", {}});
  http.responses.push_back(InitializeResponse());
  http.responses.push_back({202, "", {}});
  StreamableHttpConfig config;
  config.endpoint_url = "https://example.com/mcp";

  auto client = ConnectMcp(config, MakeClientOptions(), &http);

  ASSERT_TRUE(client.ok()) << client.status();
  EXPECT_EQ((*client)->revision(), ProtocolRevision::k2025_11_25);
  EXPECT_EQ(http.modern_calls, 1);
  EXPECT_EQ(http.bodies.size(), 3);
}

TEST(McpClientTest, PreferLatestFallsBackOnDiscoveryMethodNotFound) {
  FakeHttpClient http;
  http.responses.push_back(JsonResponse(
      R"({"jsonrpc":"2.0","id":"modern-1","error":{"code":-32601,"message":"Method not found"}})"));
  http.responses.push_back(InitializeResponse());
  http.responses.push_back({202, "", {}});
  StreamableHttpConfig config;
  config.endpoint_url = "https://example.com/mcp";

  auto client = ConnectMcp(config, MakeClientOptions(), &http);

  ASSERT_TRUE(client.ok()) << client.status();
  EXPECT_EQ((*client)->revision(), ProtocolRevision::k2025_11_25);
  EXPECT_EQ(http.modern_calls, 1);
  EXPECT_EQ(http.bodies.size(), 3);
}

TEST(McpClientTest, PreferLatestFallsBackOnUnsupportedProtocolVersion) {
  FakeHttpClient http;
  http.responses.push_back(JsonResponse(
      R"({"jsonrpc":"2.0","id":"modern-1","error":{"code":-32000,"message":"Unsupported protocol version: 2026-07-28"}})",
      400));
  http.responses.push_back(InitializeResponse());
  http.responses.push_back({202, "", {}});
  StreamableHttpConfig config;
  config.endpoint_url = "https://example.com/mcp";

  auto client = ConnectMcp(config, MakeClientOptions(), &http);

  ASSERT_TRUE(client.ok()) << client.status();
  EXPECT_EQ((*client)->revision(), ProtocolRevision::k2025_11_25);
  EXPECT_EQ(http.modern_calls, 1);
  EXPECT_EQ(http.bodies.size(), 3);
}

TEST(McpClientTest, LatestOnlyDoesNotFallbackOnUnsupportedProtocolVersion) {
  FakeHttpClient http;
  http.responses.push_back(JsonResponse(
      R"({"jsonrpc":"2.0","id":"modern-1","error":{"code":-32000,"message":"Unsupported protocol version"}})",
      400));
  StreamableHttpConfig config;
  config.endpoint_url = "https://example.com/mcp";

  auto client = ConnectMcp(config, MakeClientOptions(SelectionPolicy::kLatestOnly), &http);

  EXPECT_FALSE(client.ok());
  EXPECT_EQ(http.modern_calls, 1);
  EXPECT_EQ(http.bodies.size(), 1);
}

TEST(McpClientTest, PreferLatestFallsBackWhenDiscoveryListsOnlyClassicVersions) {
  FakeHttpClient http;
  http.responses.push_back(JsonResponse(
      R"({"jsonrpc":"2.0","id":"modern-1","result":{"supportedVersions":["2025-11-25"],"capabilities":{}}})"));
  http.responses.push_back(InitializeResponse());
  http.responses.push_back({202, "", {}});
  StreamableHttpConfig config;
  config.endpoint_url = "https://example.com/mcp";

  auto client = ConnectMcp(config, MakeClientOptions(), &http);

  ASSERT_TRUE(client.ok()) << client.status();
  EXPECT_EQ((*client)->revision(), ProtocolRevision::k2025_11_25);
  EXPECT_EQ(http.modern_calls, 1);
  EXPECT_EQ(http.bodies.size(), 3);
}

TEST(McpClientTest, DoesNotDowngradeRecognizedModernOrAuthErrors) {
  StreamableHttpConfig config;
  config.endpoint_url = "https://example.com/mcp";
  FakeHttpClient modern_error;
  modern_error.responses.push_back(JsonResponse(
      R"({"jsonrpc":"2.0","id":"modern-1","error":{"code":-32022,"message":"unsupported","data":{"supported":["2025-11-25"]}}})",
      400));
  auto unsupported = ConnectMcp(config, MakeClientOptions(), &modern_error);
  EXPECT_FALSE(unsupported.ok());
  EXPECT_EQ(modern_error.modern_calls, 1);
  EXPECT_EQ(modern_error.bodies.size(), 1);

  FakeHttpClient method_not_found_bad_request;
  method_not_found_bad_request.responses.push_back(JsonResponse(
      R"({"jsonrpc":"2.0","id":"modern-1","error":{"code":-32601,"message":"Method not found"}})", 400));
  auto bad_request = ConnectMcp(config, MakeClientOptions(), &method_not_found_bad_request);
  EXPECT_FALSE(bad_request.ok());
  EXPECT_EQ(method_not_found_bad_request.modern_calls, 1);
  EXPECT_EQ(method_not_found_bad_request.bodies.size(), 1);

  FakeHttpClient auth_error;
  auth_error.responses.push_back({401, "", {}});
  auto unauthorized = ConnectMcp(config, MakeClientOptions(), &auth_error);
  EXPECT_TRUE(absl::IsUnauthenticated(unauthorized.status()));
  EXPECT_EQ(auth_error.modern_calls, 1);
  EXPECT_EQ(auth_error.bodies.size(), 1);

  FakeHttpClient json_auth_error;
  json_auth_error.responses.push_back(JsonResponse(
      R"({"jsonrpc":"2.0","id":"modern-1","error":{"code":-32601,"message":"unauthorized"}})", 401));
  auto json_unauthorized = ConnectMcp(config, MakeClientOptions(), &json_auth_error);
  EXPECT_TRUE(absl::IsUnauthenticated(json_unauthorized.status()));
  EXPECT_EQ(json_auth_error.modern_calls, 1);
  EXPECT_EQ(json_auth_error.bodies.size(), 1);
}

TEST(McpClientTest, HonorsExplicitSelectionPolicies) {
  StreamableHttpConfig config;
  config.endpoint_url = "https://example.com/mcp";
  FakeHttpClient classic;
  classic.responses.push_back(InitializeResponse());
  classic.responses.push_back({202, "", {}});
  auto classic_client = ConnectMcp(config, MakeClientOptions(SelectionPolicy::kClassicOnly), &classic);
  ASSERT_TRUE(classic_client.ok()) << classic_client.status();
  EXPECT_EQ((*classic_client)->revision(), ProtocolRevision::k2025_11_25);
  EXPECT_EQ(classic.modern_calls, 0);

  FakeHttpClient latest;
  latest.responses.push_back({400, "", {}});
  auto latest_client = ConnectMcp(config, MakeClientOptions(SelectionPolicy::kLatestOnly), &latest);
  EXPECT_FALSE(latest_client.ok());
  EXPECT_EQ(latest.bodies.size(), 1);
}

TEST(McpClientTest, ConnectClassicStreamableHttpReturnsInitializedSession) {
  FakeHttpClient http;
  http.responses.push_back(InitializeResponse());
  http.responses.push_back({202, "", {}});
  StreamableHttpConfig config;
  config.endpoint_url = "https://example.com/mcp";

  auto session = ConnectClassicStreamableHttp(config, MakeOptions(), &http);

  ASSERT_TRUE(session.ok()) << session.status();
  EXPECT_TRUE((*session)->initialized());
  EXPECT_EQ((*session)->protocol_version(), kClassicProtocolVersion);
  EXPECT_TRUE((*session)->server_capabilities().tools);
  EXPECT_EQ(http.last_url, "https://example.com/mcp");
  ASSERT_EQ(http.bodies.size(), 2);
}

}  // namespace
}  // namespace slop::mcp
