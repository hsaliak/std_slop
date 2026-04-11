
#include "acp/rpc_envelope.h"

#include <gtest/gtest.h>

namespace slop::acp {
namespace {

TEST(RpcEnvelopeTest, ParseValidRequest) {
  auto parsed = ParseRpcRequest(R"({"jsonrpc":"2.0","id":1,"method":"x","params":{"k":"v"}})");
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(parsed->method, "x");
  ASSERT_TRUE(parsed->id.has_value());
  EXPECT_EQ((*parsed->id).get<int>(), 1);
  EXPECT_TRUE(parsed->params.is_object());
}

TEST(RpcEnvelopeTest, ParseRejectsInvalidJson) {
  const auto parsed = ParseRpcRequest("not-json");
  EXPECT_FALSE(parsed.ok());
  ASSERT_FALSE(parsed.ok());
  EXPECT_TRUE(IsRpcParseError(parsed.status()));
}

TEST(RpcEnvelopeTest, NonParseValidationErrorNotClassifiedAsParseError) {
  const auto parsed = ParseRpcRequest("[]");
  ASSERT_FALSE(parsed.ok());
  EXPECT_FALSE(IsRpcParseError(parsed.status()));
}

TEST(RpcEnvelopeTest, ParseRejectsNonObject) {
  auto parsed = ParseRpcRequest("[]");
  EXPECT_FALSE(parsed.ok());
}

TEST(RpcEnvelopeTest, ParseRejectsWrongVersion) {
  auto parsed = ParseRpcRequest(R"({"jsonrpc":"1.0","id":1,"method":"x"})");
  EXPECT_FALSE(parsed.ok());
}

TEST(RpcEnvelopeTest, ParseRejectsMissingMethod) {
  auto parsed = ParseRpcRequest(R"({"jsonrpc":"2.0","id":1})");
  EXPECT_FALSE(parsed.ok());
}

TEST(RpcEnvelopeTest, ParseNotificationHasNoResponseId) {
  auto parsed = ParseRpcRequest(R"({"jsonrpc":"2.0","method":"x"})");
  ASSERT_TRUE(parsed.ok());
  EXPECT_TRUE(parsed->is_notification());
}

TEST(RpcEnvelopeTest, ParseDefaultsParamsToObject) {
  auto parsed = ParseRpcRequest(R"({"jsonrpc":"2.0","id":"a","method":"x"})");
  ASSERT_TRUE(parsed.ok());
  EXPECT_TRUE(parsed->params.is_object());
}

TEST(RpcEnvelopeTest, ParseRejectsObjectId) {
  auto parsed = ParseRpcRequest(R"({"jsonrpc":"2.0","id":{},"method":"x"})");
  EXPECT_FALSE(parsed.ok());
}

TEST(RpcEnvelopeTest, MakeMethodNotFoundResponseShape) {
  nlohmann::json id = "abc";
  auto response = MakeMethodNotFoundResponse(id, "nope");
  EXPECT_EQ(response.at("jsonrpc").get<std::string>(), "2.0");
  EXPECT_EQ(response.at("id").get<std::string>(), "abc");
  EXPECT_EQ(response.at("error").at("code").get<int>(), kMethodNotFoundCode);
}

TEST(RpcEnvelopeTest, MakeParseErrorSetsNullId) {
  auto response = MakeParseErrorResponse();
  EXPECT_EQ(response.at("jsonrpc").get<std::string>(), "2.0");
  EXPECT_TRUE(response.at("id").is_null());
  EXPECT_EQ(response.at("error").at("code").get<int>(), kParseErrorCode);
}

}  // namespace
}  // namespace slop::acp

TEST(RpcEnvelopeTest, ParseRejectsScalarParams) {
  auto parsed = slop::acp::ParseRpcRequest(R"({"jsonrpc":"2.0","id":1,"method":"x","params":42})");
  EXPECT_FALSE(parsed.ok());
}

TEST(RpcEnvelopeTest, ParseAllowsArrayParams) {
  auto parsed = slop::acp::ParseRpcRequest(R"({"jsonrpc":"2.0","id":1,"method":"x","params":[1,2]})");
  EXPECT_TRUE(parsed.ok());
}