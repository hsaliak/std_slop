#include "mcp/json_rpc.h"

#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "core/json_utils.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace slop::mcp {
namespace {

TEST(JsonRpcTest, BuildsRequestWithIntegerId) {
  const nlohmann::json request = BuildJsonRpcRequest(int64_t{7}, "tools/list", {{"cursor", "abc"}});

  EXPECT_EQ(json_get_or(request, "jsonrpc", std::string{}), "2.0");
  EXPECT_EQ(json_get_or(request, "method", std::string{}), "tools/list");
  EXPECT_EQ(json_get_or(request, "id", 0), 7);
  ASSERT_TRUE(request.contains("params"));
  EXPECT_EQ(json_get_or(request["params"], "cursor", std::string{}), "abc");
}

TEST(JsonRpcTest, BuildsNotificationWithoutId) {
  const nlohmann::json notification = BuildJsonRpcNotification("notifications/initialized");

  EXPECT_EQ(json_get_or(notification, "jsonrpc", std::string{}), "2.0");
  EXPECT_EQ(json_get_or(notification, "method", std::string{}), "notifications/initialized");
  EXPECT_FALSE(notification.contains("id"));
  EXPECT_FALSE(notification.contains("params"));
}

TEST(JsonRpcTest, ParsesSuccessResponse) {
  const nlohmann::json raw = {{"jsonrpc", "2.0"}, {"id", "abc"}, {"result", {{"ok", true}}}};

  auto parsed = ParseJsonRpcResponse(raw);

  ASSERT_TRUE(parsed.ok()) << parsed.status();
  EXPECT_EQ(JsonRpcIdToString(parsed->id), "abc");
  ASSERT_TRUE(parsed->result.has_value());
  EXPECT_TRUE(json_get_or(*parsed->result, "ok", false));
  EXPECT_FALSE(parsed->error.has_value());
}

TEST(JsonRpcTest, ParsesErrorResponse) {
  const nlohmann::json raw = {
      {"jsonrpc", "2.0"}, {"id", 1}, {"error", {{"code", -32602}, {"message", "bad params"}, {"data", {{"field", "x"}}}}}};

  auto parsed = ParseJsonRpcResponse(raw);

  ASSERT_TRUE(parsed.ok()) << parsed.status();
  EXPECT_EQ(JsonRpcIdToString(parsed->id), "1");
  ASSERT_TRUE(parsed->error.has_value());
  EXPECT_EQ(parsed->error->code, -32602);
  EXPECT_EQ(parsed->error->message, "bad params");
  EXPECT_EQ(json_get_or(parsed->error->data, "field", std::string{}), "x");
  EXPECT_FALSE(parsed->result.has_value());
}

TEST(JsonRpcTest, ParsesNullIdErrorResponse) {
  const nlohmann::json raw = {{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32600}, {"message", "bad request"}}}};

  auto parsed = ParseJsonRpcResponse(raw);

  ASSERT_TRUE(parsed.ok()) << parsed.status();
  EXPECT_EQ(JsonRpcIdToString(parsed->id), "null");
  ASSERT_TRUE(parsed->error.has_value());
  EXPECT_EQ(parsed->error->code, -32600);
}

TEST(JsonRpcTest, RejectsMissingJsonRpcVersion) {
  const auto parsed = ParseJsonRpcResponse({{"id", 1}, {"result", nlohmann::json::object()}});

  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(JsonRpcTest, RejectsWrongJsonRpcVersion) {
  const auto parsed = ParseJsonRpcResponse({{"jsonrpc", "1.0"}, {"id", 1}, {"result", nlohmann::json::object()}});

  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(JsonRpcTest, RejectsResponseWithResultAndError) {
  const auto parsed = ParseJsonRpcResponse(
      {{"jsonrpc", "2.0"}, {"id", 1}, {"result", nlohmann::json::object()}, {"error", nlohmann::json::object()}});

  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(JsonRpcTest, RejectsRequestWithoutMethod) {
  const auto parsed = ParseJsonRpcMessage(R"({"jsonrpc":"2.0","params":{}})");

  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(JsonRpcTest, RejectsRequestWithResultField) {
  const auto parsed = ParseJsonRpcMessage(R"({"jsonrpc":"2.0","id":1,"method":"ping","result":{}})");

  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(JsonRpcTest, RejectsRequestWithInvalidIdShape) {
  const auto parsed = ParseJsonRpcMessage(R"({"jsonrpc":"2.0","id":{},"method":"ping"})");

  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(JsonRpcTest, RejectsMalformedErrorResponse) {
  const auto parsed = ParseJsonRpcMessage(R"({"jsonrpc":"2.0","id":1,"error":{"code":"bad","message":4}})");

  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(JsonRpcTest, ParsesGenericRequestMessage) {
  auto parsed = ParseJsonRpcMessage(R"({"jsonrpc":"2.0","id":2,"method":"ping"})");

  ASSERT_TRUE(parsed.ok()) << parsed.status();
  EXPECT_EQ(json_get_or(*parsed, "method", std::string{}), "ping");
}

}  // namespace
}  // namespace slop::mcp
