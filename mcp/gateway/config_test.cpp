#include "mcp/gateway/config.h"

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace slop::mcp::gateway {
namespace {

nlohmann::json Config() {
  return {{"servers",
           {{{"alias", "echo"},
             {"transport", "stdio"},
             {"command", "/usr/bin/echo"},
             {"args", nlohmann::json::array()},
             {"allowTools", {"echo"}}}}}};
}

TEST(ConfigTest, ParsesBoundedStdioConfiguration) {
  auto config = ParseConfig(Config());
  ASSERT_TRUE(config.ok()) << config.status();
  ASSERT_EQ(config->servers.size(), 1);
  EXPECT_EQ(config->servers[0].alias, "echo");
  EXPECT_EQ(config->servers[0].allow_tools, std::vector<std::string>({"echo"}));
}

TEST(ConfigTest, BoundsRunTimeout) {
  nlohmann::json value = {{"servers", nlohmann::json::array()}, {"runTimeoutMs", 250}};
  auto config = ParseConfig(value);
  ASSERT_TRUE(config.ok()) << config.status();
  EXPECT_EQ(config->run_timeout_ms, 250);

  for (const nlohmann::json& timeout : {nlohmann::json(0), nlohmann::json(60'001), nlohmann::json("100")}) {
    value["runTimeoutMs"] = timeout;
    EXPECT_FALSE(ParseConfig(value).ok());
  }
}

TEST(ConfigTest, EmptyAndOmittedGrantsUseOpenDefault) {
  nlohmann::json empty = Config();
  empty["servers"][0]["allowTools"] = nlohmann::json::array();
  auto empty_config = ParseConfig(empty);
  ASSERT_TRUE(empty_config.ok()) << empty_config.status();
  EXPECT_TRUE(empty_config->servers[0].allow_tools.empty());

  nlohmann::json omitted = Config();
  omitted["servers"][0].erase("allowTools");
  auto omitted_config = ParseConfig(omitted);
  ASSERT_TRUE(omitted_config.ok()) << omitted_config.status();
  EXPECT_TRUE(omitted_config->servers[0].allow_tools.empty());
}

TEST(ConfigTest, RejectsInvalidShapeAndTransport) {
  for (const nlohmann::json& value : {nlohmann::json::array(), nlohmann::json{{"servers", "bad"}},
                                      nlohmann::json{{"servers", nlohmann::json::array()}, {"extra", 1}}}) {
    EXPECT_FALSE(ParseConfig(value).ok());
  }
  nlohmann::json http = Config();
  http["servers"][0]["transport"] = "http";
  EXPECT_FALSE(ParseConfig(http).ok());
}

TEST(ConfigTest, RejectsAliasesThatCouldCollideOrEscapeProxyScope) {
  for (const std::string& alias :
       {"mcp", "input", "Object", "await", "bad-name", "1server", "__proto__", "__slop_catalog", "parseInt",
        "decodeURI", "console", "SharedArrayBuffer", "toString", "hasOwnProperty"}) {
    nlohmann::json value = Config();
    value["servers"][0]["alias"] = alias;
    EXPECT_FALSE(ParseConfig(value).ok()) << alias;
  }
}

TEST(ConfigTest, RejectsDuplicateAliasesAndGrants) {
  nlohmann::json duplicate_alias = Config();
  duplicate_alias["servers"].push_back(duplicate_alias["servers"][0]);
  EXPECT_FALSE(ParseConfig(duplicate_alias).ok());

  nlohmann::json duplicate_grant = Config();
  duplicate_grant["servers"][0]["allowTools"] = {"echo", "echo"};
  EXPECT_FALSE(ParseConfig(duplicate_grant).ok());
}

TEST(ConfigTest, RejectsMixedOrMalformedStdioFields) {
  nlohmann::json missing = Config();
  missing["servers"][0].erase("args");
  EXPECT_FALSE(ParseConfig(missing).ok());

  nlohmann::json relative = Config();
  relative["servers"][0]["command"] = "echo";
  EXPECT_FALSE(ParseConfig(relative).ok());

  nlohmann::json invalid_args = Config();
  invalid_args["servers"][0]["args"] = {3};
  EXPECT_FALSE(ParseConfig(invalid_args).ok());

  nlohmann::json mixed = Config();
  mixed["servers"][0]["url"] = "https://example.invalid";
  EXPECT_FALSE(ParseConfig(mixed).ok());
}

TEST(ConfigTest, TextParserRejectsMalformedAndOversizedInput) {
  EXPECT_FALSE(ParseConfigText("not-json").ok());
  EXPECT_FALSE(ParseConfigText(std::string(1024 * 1024 + 1, ' ')).ok());
  auto config = ParseConfigText(R"({"servers":[]})");
  ASSERT_TRUE(config.ok()) << config.status();
  EXPECT_TRUE(config->servers.empty());
}

}  // namespace
}  // namespace slop::mcp::gateway
