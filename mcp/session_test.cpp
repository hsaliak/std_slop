#include "mcp/session.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/time/time.h"
#include "core/json_utils.h"
#include "gtest/gtest.h"
#include "mcp/json_rpc.h"
#include "mcp/protocol.h"
#include "mcp/transport.h"
#include "nlohmann/json.hpp"

namespace slop::mcp {
namespace {

class FakeTransport : public Transport {
 public:
  absl::Status Start() override {
    started = true;
    return start_status;
  }

  absl::Status Send(const nlohmann::json& message) override {
    sent.push_back(message);
    return send_status;
  }

  absl::StatusOr<nlohmann::json> Receive(absl::Duration /*timeout*/) override {
    if (!receive_status.ok()) return receive_status;
    if (responses.empty()) return absl::UnavailableError("no response queued");
    nlohmann::json response = responses.front();
    responses.erase(responses.begin());
    return response;
  }

  absl::Status Close() override {
    closed = true;
    return close_status;
  }

  bool started = false;
  bool closed = false;
  absl::Status start_status = absl::OkStatus();
  absl::Status send_status = absl::OkStatus();
  absl::Status receive_status = absl::OkStatus();
  absl::Status close_status = absl::OkStatus();
  std::vector<nlohmann::json> sent;
  std::vector<nlohmann::json> responses;
};

InitializeOptions MakeOptions() {
  InitializeOptions options;
  options.client_info.name = "test-client";
  options.client_info.version = "1.0";
  options.request_timeout = absl::Seconds(1);
  return options;
}

nlohmann::json InitializeResult(nlohmann::json capabilities = {{"tools", {{"listChanged", true}}}}) {
  return {{"jsonrpc", "2.0"},
          {"id", 1},
          {"result", {{"protocolVersion", std::string(kLatestProtocolVersion)}, {"capabilities", std::move(capabilities)}}}};
}

TEST(SessionTest, InitializeStartsTransportAndSendsInitializedNotification) {
  auto fake = std::make_unique<FakeTransport>();
  FakeTransport* raw = fake.get();
  raw->responses.push_back(InitializeResult());
  Session session(std::move(fake));

  ASSERT_TRUE(session.Initialize(MakeOptions()).ok());

  EXPECT_TRUE(raw->started);
  ASSERT_EQ(raw->sent.size(), 2);
  EXPECT_EQ(json_get_or(raw->sent[0], "method", std::string{}), "initialize");
  EXPECT_EQ(json_get_or(raw->sent[1], "method", std::string{}), "notifications/initialized");
  EXPECT_TRUE(session.initialized());
  EXPECT_EQ(session.protocol_version(), kLatestProtocolVersion);
  EXPECT_TRUE(session.server_capabilities().tools);
  EXPECT_TRUE(session.server_capabilities().tools_list_changed);
}

TEST(SessionTest, InitializeRejectsUnsupportedVersion) {
  auto fake = std::make_unique<FakeTransport>();
  fake->responses.push_back({{"jsonrpc", "2.0"},
                             {"id", 1},
                             {"result", {{"protocolVersion", "1900-01-01"}, {"capabilities", nlohmann::json::object()}}}});
  Session session(std::move(fake));

  auto status = session.Initialize(MakeOptions());

  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.code(), absl::StatusCode::kUnimplemented);
}

TEST(SessionTest, InitializeRejectsMalformedResult) {
  auto fake = std::make_unique<FakeTransport>();
  fake->responses.push_back({{"jsonrpc", "2.0"}, {"id", 1}, {"result", {{"protocolVersion", std::string(kLatestProtocolVersion)}}}});
  Session session(std::move(fake));

  auto status = session.Initialize(MakeOptions());

  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
}

TEST(SessionTest, PingBeforeInitializeRejected) {
  Session session(std::make_unique<FakeTransport>());

  auto status = session.Ping();

  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.code(), absl::StatusCode::kFailedPrecondition);
}

TEST(SessionTest, PingSendsRequestAfterInitialize) {
  auto fake = std::make_unique<FakeTransport>();
  FakeTransport* raw = fake.get();
  raw->responses.push_back(InitializeResult(nlohmann::json::object()));
  raw->responses.push_back({{"jsonrpc", "2.0"}, {"id", 2}, {"result", nlohmann::json::object()}});
  Session session(std::move(fake));

  ASSERT_TRUE(session.Initialize(MakeOptions()).ok());
  ASSERT_TRUE(session.Ping().ok());

  ASSERT_EQ(raw->sent.size(), 3);
  EXPECT_EQ(json_get_or(raw->sent[2], "method", std::string{}), "ping");
  EXPECT_EQ(json_get_or(raw->sent[2], "id", 0), 2);
}

TEST(SessionTest, CloseClosesTransport) {
  auto fake = std::make_unique<FakeTransport>();
  FakeTransport* raw = fake.get();
  Session session(std::move(fake));

  ASSERT_TRUE(session.Close().ok());

  EXPECT_TRUE(raw->closed);
}

TEST(SessionTest, ListToolsParsesTools) {
  auto fake = std::make_unique<FakeTransport>();
  FakeTransport* raw = fake.get();
  raw->responses.push_back(InitializeResult({{"tools", nlohmann::json::object()}}));
  const nlohmann::json tool = {{"name", "search"},
                               {"title", "Search"},
                               {"description", "Search things"},
                               {"inputSchema", {{"type", "object"}}},
                               {"outputSchema", {{"type", "object"}}},
                               {"annotations", {{"readOnlyHint", true}}}};
  raw->responses.push_back({{"jsonrpc", "2.0"},
                            {"id", 2},
                            {"result", {{"tools", nlohmann::json::array({tool})}}}});
  Session session(std::move(fake));

  ASSERT_TRUE(session.Initialize(MakeOptions()).ok());
  auto tools = session.ListTools();

  ASSERT_TRUE(tools.ok()) << tools.status();
  ASSERT_EQ(tools->size(), 1);
  EXPECT_EQ((*tools)[0].name, "search");
  ASSERT_TRUE((*tools)[0].title.has_value());
  EXPECT_EQ(*(*tools)[0].title, "Search");
  ASSERT_TRUE((*tools)[0].description.has_value());
  EXPECT_EQ(*(*tools)[0].description, "Search things");
  EXPECT_EQ(json_get_or((*tools)[0].input_schema, "type", std::string{}), "object");
  EXPECT_EQ(json_get_or((*tools)[0].output_schema, "type", std::string{}), "object");
  EXPECT_TRUE(json_get_or((*tools)[0].annotations, "readOnlyHint", false));
  ASSERT_EQ(raw->sent.size(), 3);
  EXPECT_EQ(json_get_or(raw->sent[2], "method", std::string{}), "tools/list");
}

TEST(SessionTest, ListToolsFollowsPagination) {
  auto fake = std::make_unique<FakeTransport>();
  FakeTransport* raw = fake.get();
  raw->responses.push_back(InitializeResult({{"tools", nlohmann::json::object()}}));
  const nlohmann::json first_tool = {{"name", "first"}, {"inputSchema", {{"type", "object"}}}};
  const nlohmann::json second_tool = {{"name", "second"}, {"inputSchema", {{"type", "object"}}}};
  raw->responses.push_back({{"jsonrpc", "2.0"},
                            {"id", 2},
                            {"result", {{"tools", nlohmann::json::array({first_tool})}, {"nextCursor", "next"}}}});
  raw->responses.push_back({{"jsonrpc", "2.0"},
                            {"id", 3},
                            {"result", {{"tools", nlohmann::json::array({second_tool})}}}});
  Session session(std::move(fake));

  ASSERT_TRUE(session.Initialize(MakeOptions()).ok());
  auto tools = session.ListTools();

  ASSERT_TRUE(tools.ok()) << tools.status();
  ASSERT_EQ(tools->size(), 2);
  EXPECT_EQ((*tools)[0].name, "first");
  EXPECT_EQ((*tools)[1].name, "second");
  ASSERT_EQ(raw->sent.size(), 4);
  ASSERT_TRUE(raw->sent[3].contains("params"));
  EXPECT_EQ(json_get_or(raw->sent[3]["params"], "cursor", std::string{}), "next");
}

TEST(SessionTest, ListToolsRejectsMalformedTool) {
  auto fake = std::make_unique<FakeTransport>();
  fake->responses.push_back(InitializeResult({{"tools", nlohmann::json::object()}}));
  fake->responses.push_back({{"jsonrpc", "2.0"},
                             {"id", 2},
                             {"result", {{"tools", nlohmann::json::array({{{"name", "bad"}}})}}}});
  Session session(std::move(fake));

  ASSERT_TRUE(session.Initialize(MakeOptions()).ok());
  auto tools = session.ListTools();

  ASSERT_FALSE(tools.ok());
  EXPECT_EQ(tools.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(SessionTest, CallToolParsesSuccessResult) {
  auto fake = std::make_unique<FakeTransport>();
  FakeTransport* raw = fake.get();
  raw->responses.push_back(InitializeResult({{"tools", nlohmann::json::object()}}));
  raw->responses.push_back({{"jsonrpc", "2.0"},
                            {"id", 2},
                            {"result",
                             {{"content", nlohmann::json::array({{{"type", "text"}, {"text", "ok"}}})},
                              {"structuredContent", {{"answer", 42}}}}}});
  Session session(std::move(fake));

  ASSERT_TRUE(session.Initialize(MakeOptions()).ok());
  auto result = session.CallTool("search", {{"query", "mcp"}});

  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_FALSE(result->is_error);
  ASSERT_EQ(result->content.size(), 1);
  EXPECT_EQ(json_get_or(result->content[0], "text", std::string{}), "ok");
  EXPECT_EQ(json_get_or(result->structured_content, "answer", 0), 42);
  ASSERT_EQ(raw->sent.size(), 3);
  EXPECT_EQ(json_get_or(raw->sent[2], "method", std::string{}), "tools/call");
  ASSERT_TRUE(raw->sent[2].contains("params"));
  EXPECT_EQ(json_get_or(raw->sent[2]["params"], "name", std::string{}), "search");
}

TEST(SessionTest, CallToolPreservesIsError) {
  auto fake = std::make_unique<FakeTransport>();
  fake->responses.push_back(InitializeResult({{"tools", nlohmann::json::object()}}));
  fake->responses.push_back({{"jsonrpc", "2.0"},
                             {"id", 2},
                             {"result", {{"content", nlohmann::json::array()}, {"isError", true}}}});
  Session session(std::move(fake));

  ASSERT_TRUE(session.Initialize(MakeOptions()).ok());
  auto result = session.CallTool("search", nlohmann::json::object());

  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_TRUE(result->is_error);
}

TEST(SessionTest, CallToolRejectsNonObjectArguments) {
  auto fake = std::make_unique<FakeTransport>();
  fake->responses.push_back(InitializeResult({{"tools", nlohmann::json::object()}}));
  Session session(std::move(fake));

  ASSERT_TRUE(session.Initialize(MakeOptions()).ok());
  auto result = session.CallTool("search", nlohmann::json::array());

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace slop::mcp
