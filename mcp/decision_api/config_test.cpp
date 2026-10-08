#include "mcp/decision_api/config.h"

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace slop::mcp::decision_api {
namespace {

nlohmann::json MinimalConfig() { return {{"apiKey", "test-key"}, {"model", "jev-latest"}}; }

TEST(DecisionApiConfigTest, DefaultsToOpenRouterAndForwardsAlias) {
  auto config = ParseConfig(MinimalConfig());
  ASSERT_TRUE(config.ok()) << config.status();
  EXPECT_EQ(config->endpoint, kDefaultEndpoint);
  ASSERT_TRUE(config->models_endpoint.has_value());
  EXPECT_EQ(*config->models_endpoint, kDefaultModelsEndpoint);
  EXPECT_EQ(config->model, "jev-latest");
  EXPECT_EQ(config->timeout_ms, 30000);
}

TEST(DecisionApiConfigTest, CustomEndpointDisablesDefaultCatalog) {
  nlohmann::json value = MinimalConfig();
  value["endpoint"] = "https://example.test/decisions";
  auto config = ParseConfig(value);
  ASSERT_TRUE(config.ok()) << config.status();
  EXPECT_EQ(config->endpoint, "https://example.test/decisions");
  EXPECT_FALSE(config->models_endpoint.has_value());
  value["modelsEndpoint"] = "https://example.test/models";
  config = ParseConfig(value);
  ASSERT_TRUE(config.ok()) << config.status();
  EXPECT_EQ(config->models_endpoint, "https://example.test/models");
}

TEST(DecisionApiConfigTest, RejectsMissingFieldsUnknownFieldsAndWrongTypes) {
  EXPECT_FALSE(ParseConfig(nlohmann::json::object()).ok());
  nlohmann::json value = MinimalConfig();
  value["unexpected"] = true;
  EXPECT_FALSE(ParseConfig(value).ok());
  value = MinimalConfig();
  value["model"] = 42;
  EXPECT_FALSE(ParseConfig(value).ok());
  value = MinimalConfig();
  value["apiKey"] = "bad\r\nHeader: value";
  EXPECT_FALSE(ParseConfig(value).ok());
}

TEST(DecisionApiConfigTest, RejectsInvalidEndpointsAndTimeouts) {
  nlohmann::json value = MinimalConfig();
  value["endpoint"] = "http://example.test/decisions";
  EXPECT_FALSE(ParseConfig(value).ok());
  value["endpoint"] = "https://user@example.test/decisions";
  EXPECT_FALSE(ParseConfig(value).ok());
  value["endpoint"] = "https:///missing-host";
  EXPECT_FALSE(ParseConfig(value).ok());
  value["endpoint"] = "https://example.test /path";
  EXPECT_FALSE(ParseConfig(value).ok());
  value["endpoint"] = std::string("https://example.test/\0path", 25);
  EXPECT_FALSE(ParseConfig(value).ok());
  value = MinimalConfig();
  value["timeoutMs"] = 60001;
  EXPECT_FALSE(ParseConfig(value).ok());
  value["timeoutMs"] = 1.5;
  EXPECT_FALSE(ParseConfig(value).ok());
}

TEST(DecisionApiConfigTest, RejectsMalformedAndOversizedJson) {
  EXPECT_FALSE(ParseConfigText("{oops").ok());
  EXPECT_FALSE(ParseConfigText(std::string(64 * 1024 + 1, ' ')).ok());
}

}  // namespace
}  // namespace slop::mcp::decision_api
