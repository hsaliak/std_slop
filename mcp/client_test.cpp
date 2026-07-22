#include "mcp/client.h"

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "core/http_client.h"
#include "gtest/gtest.h"
#include "mcp/protocol.h"
#include "nlohmann/json.hpp"

namespace slop::mcp {
namespace {

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

TEST(McpClientTest, ConnectStreamableHttpRejectsNullHttpClient) {
  StreamableHttpConfig config;
  config.endpoint_url = "https://example.com/mcp";

  auto session = ConnectStreamableHttp(config, MakeOptions(), nullptr);

  ASSERT_FALSE(session.ok());
  EXPECT_EQ(session.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(McpClientTest, ConnectStreamableHttpRejectsEmptyEndpoint) {
  FakeHttpClient http;
  StreamableHttpConfig config;
  http.responses.push_back(InitializeResponse());

  auto session = ConnectStreamableHttp(config, MakeOptions(), &http);

  ASSERT_FALSE(session.ok());
  EXPECT_EQ(session.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(McpClientTest, ConnectStreamableHttpReturnsInitializedSession) {
  FakeHttpClient http;
  http.responses.push_back(InitializeResponse());
  http.responses.push_back({202, "", {}});
  StreamableHttpConfig config;
  config.endpoint_url = "https://example.com/mcp";

  auto session = ConnectStreamableHttp(config, MakeOptions(), &http);

  ASSERT_TRUE(session.ok()) << session.status();
  EXPECT_TRUE((*session)->initialized());
  EXPECT_EQ((*session)->protocol_version(), kLatestProtocolVersion);
  EXPECT_TRUE((*session)->server_capabilities().tools);
  EXPECT_EQ(http.last_url, "https://example.com/mcp");
  ASSERT_EQ(http.bodies.size(), 2);
}

}  // namespace
}  // namespace slop::mcp
