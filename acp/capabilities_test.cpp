
#include "acp/capabilities.h"

#include <gtest/gtest.h>

namespace slop::acp {
namespace {

TEST(CapabilitiesTest, ParseInitializeParamsValid) {
  nlohmann::json params = {
      {"protocolVersion", "1"},
      {"capabilities", nlohmann::json::object()},
      {"runtimeOptions", nlohmann::json({{"trace", true}})},
  };
  auto parsed_or = ParseInitializeParams(params);
  ASSERT_TRUE(parsed_or.ok());
  EXPECT_EQ(parsed_or->protocol_version, "1");
  EXPECT_TRUE(parsed_or->client_capabilities.is_object());
  EXPECT_TRUE(parsed_or->runtime_options.is_object());
}

TEST(CapabilitiesTest, ParseInitializeParamsRejectsUnsupportedVersion) {
  nlohmann::json params = {
      {"protocolVersion", "2"},
      {"capabilities", nlohmann::json::object()},
  };
  auto parsed_or = ParseInitializeParams(params);
  ASSERT_FALSE(parsed_or.ok());
  EXPECT_EQ(parsed_or.status().message(), "unsupported_protocol_version");
}

TEST(CapabilitiesTest, ParseInitializeParamsRejectsMissingCapabilities) {
  nlohmann::json params = {
      {"protocolVersion", "1"},
  };
  auto parsed_or = ParseInitializeParams(params);
  ASSERT_FALSE(parsed_or.ok());
  EXPECT_EQ(parsed_or.status().message(), "initialize_capabilities_must_be_object");
}

TEST(CapabilitiesTest, ApplyInitializeRequestPersistsState) {
  InitializeRequest req;
  req.protocol_version = "1";
  req.client_capabilities = nlohmann::json({{"roots", true}});
  req.runtime_options = nlohmann::json({{"trace", false}});

  NegotiatedRuntimeOptions state;
  ApplyInitializeRequest(req, &state);

  EXPECT_TRUE(state.initialized);
  EXPECT_EQ(state.protocol_version, "1");
  EXPECT_EQ(state.client_capabilities.at("roots").get<bool>(), true);
  EXPECT_EQ(state.runtime_options.at("trace").get<bool>(), false);
}

TEST(CapabilitiesTest, BuildInitializeResultHasStableProtocolAndSessionCapabilities) {
  auto result = BuildInitializeResult();
  EXPECT_EQ(result.at("protocolVersion").get<std::string>(), "1");
  ASSERT_TRUE(result.contains("capabilities"));
  ASSERT_TRUE(result.at("capabilities").contains("session"));
  EXPECT_TRUE(result.at("capabilities").at("session").at("new").get<bool>());
  EXPECT_TRUE(result.at("capabilities").at("session").at("prompt").get<bool>());
  EXPECT_TRUE(result.at("capabilities").at("session").at("cancel").get<bool>());
  EXPECT_TRUE(result.at("capabilities").at("session").at("update").get<bool>());
}

}  // namespace
}  // namespace slop::acp