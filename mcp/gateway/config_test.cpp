#include "mcp/gateway/config.h"

#include <utility>

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
  EXPECT_EQ(config->servers[0].transport, ServerTransport::kStdio);
  EXPECT_EQ(config->servers[0].allow_tools, std::vector<std::string>({"echo"}));
}

TEST(ConfigTest, ParsesHttpConfiguration) {
  for (const std::string& endpoint : {"https://example.com/mcp", "http://127.0.0.1:8123/mcp", "http://[::1]:8123/mcp",
                                      "http://[::ffff:192.0.2.1]/mcp"}) {
    nlohmann::json value = {{"servers", {{{"alias", "docs"}, {"transport", "http"}, {"endpointUrl", endpoint}}}}};
    auto config = ParseConfig(value);
    ASSERT_TRUE(config.ok()) << config.status();
    ASSERT_EQ(config->servers.size(), 1);
    EXPECT_EQ(config->servers[0].transport, ServerTransport::kHttp);
    EXPECT_EQ(config->servers[0].endpoint_url, endpoint);
    EXPECT_TRUE(config->servers[0].allow_tools.empty());
  }
}

TEST(ConfigTest, ParsesBearerAuthenticationProfiles) {
  nlohmann::json token_file = {
      {"servers", {{{"alias", "docs"}, {"transport", "http"}, {"endpointUrl", "https://example.com/mcp"},
                    {"auth", {{"type", "bearer"}, {"tokenFile", "/tmp/mcp-token.json"}}}}}}};
  auto file_config = ParseConfig(token_file);
  ASSERT_TRUE(file_config.ok()) << file_config.status();
  const HttpAuthConfig& file_auth = file_config->servers[0].auth;
  EXPECT_EQ(file_auth.method, HttpAuthMethod::kBearer);
  EXPECT_EQ(file_auth.token_file, "/tmp/mcp-token.json");
  EXPECT_TRUE(file_auth.token_env.empty());

  nlohmann::json token_env = {
      {"servers", {{{"alias", "docs"}, {"transport", "http"}, {"endpointUrl", "https://example.com/mcp"},
                    {"auth", {{"type", "bearer"}, {"tokenEnv", "DOCS_MCP_TOKEN"}}}}}}};
  auto env_config = ParseConfig(token_env);
  ASSERT_TRUE(env_config.ok()) << env_config.status();
  EXPECT_EQ(env_config->servers[0].auth.method, HttpAuthMethod::kBearer);
  EXPECT_EQ(env_config->servers[0].auth.token_env, "DOCS_MCP_TOKEN");
}

TEST(ConfigTest, ParsesOAuthAuthenticationProfile) {
  nlohmann::json value = {
      {"servers",
       {{{"alias", "docs"},
         {"transport", "http"},
         {"endpointUrl", "https://example.com/mcp"},
         {"auth", {{"type", "oauth"},
                   {"tokenFile", "/tmp/docs-token.json"},
                   {"clientId", "docs-client"},
                   {"issuer", "https://issuer.example"},
                   {"scopes", {"read", "write"}},
                   {"resourceMetadataUrl", "https://example.com/.well-known/resource"},
                   {"authorizationEndpoint", "https://issuer.example/authorize"},
                   {"tokenEndpoint", "https://issuer.example/token"},
                   {"authorizationResponseIssParameterSupported", true}}}}}}};

  auto config = ParseConfig(value);
  ASSERT_TRUE(config.ok()) << config.status();
  const HttpAuthConfig& auth = config->servers[0].auth;
  EXPECT_EQ(auth.method, HttpAuthMethod::kOAuth);
  EXPECT_EQ(auth.token_file, "/tmp/docs-token.json");
  EXPECT_EQ(auth.client_id, "docs-client");
  EXPECT_EQ(auth.expected_issuer, "https://issuer.example");
  EXPECT_EQ(auth.scopes, std::vector<std::string>({"read", "write"}));
  EXPECT_EQ(auth.resource_metadata_url, "https://example.com/.well-known/resource");
  EXPECT_EQ(auth.authorization_endpoint, "https://issuer.example/authorize");
  EXPECT_EQ(auth.token_endpoint, "https://issuer.example/token");
  EXPECT_TRUE(auth.authorization_response_iss_parameter_supported);
}

TEST(ConfigTest, RejectsInvalidAuthenticationProfiles) {
  const std::string endpoint = "https://example.com/mcp";
  const auto parse_auth = [](nlohmann::json auth, const std::string& url = "https://example.com/mcp") {
    nlohmann::json value = {{"servers", {{{"alias", "docs"}, {"transport", "http"}, {"endpointUrl", url},
                                          {"auth", std::move(auth)}}}}};
    return ParseConfig(value);
  };

  for (const nlohmann::json& auth : {nlohmann::json::object(),
                                     nlohmann::json{{"type", "api_key"}},
                                     nlohmann::json{{"type", "none"}, {"tokenEnv", "TOKEN"}},
                                     nlohmann::json{{"type", "bearer"}},
                                     nlohmann::json{{"type", "bearer"}, {"tokenFile", "/tmp/token"},
                                                    {"tokenEnv", "TOKEN"}},
                                     nlohmann::json{{"type", "bearer"}, {"tokenFile", "relative-token"}},
                                     nlohmann::json{{"type", "bearer"}, {"tokenEnv", "INVALID-NAME"}},
                                     nlohmann::json{{"type", "oauth"}, {"tokenFile", "/tmp/token"},
                                                    {"issuer", "https://issuer.example"}},
                                     nlohmann::json{{"type", "oauth"}, {"tokenFile", "/tmp/token"},
                                                    {"clientId", "client"}, {"issuer", "http://issuer.example"}},
                                     nlohmann::json{{"type", "oauth"}, {"tokenFile", "/tmp/token"},
                                                    {"clientId", "client"}, {"issuer", "https://issuer.example"},
                                                    {"authorizationEndpoint", "https://issuer.example/authorize"}}}) {
    EXPECT_FALSE(parse_auth(auth).ok()) << auth;
  }
  EXPECT_FALSE(parse_auth({{"type", "bearer"}, {"tokenEnv", "TOKEN"}}, "http://example.com/mcp").ok());
  EXPECT_FALSE(parse_auth({{"type", "oauth"}, {"tokenFile", "/tmp/token"}, {"clientId", "client"},
                           {"issuer", "https://issuer.example"}},
                          "http://example.com/mcp")
                   .ok());

  nlohmann::json stdio = Config();
  stdio["servers"][0]["auth"] = {{"type", "none"}};
  EXPECT_FALSE(ParseConfig(stdio).ok());

  const auto default_auth = ParseConfig({{"servers", {{{"alias", "docs"}, {"transport", "http"},
                                                        {"endpointUrl", endpoint}}}}});
  ASSERT_TRUE(default_auth.ok()) << default_auth.status();
  EXPECT_EQ(default_auth->servers[0].auth.method, HttpAuthMethod::kNone);
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

TEST(ConfigTest, ParsesOptionalTraceLogPath) {
  auto disabled = ParseConfig(Config());
  ASSERT_TRUE(disabled.ok()) << disabled.status();
  EXPECT_FALSE(disabled->trace_log_path.has_value());

  nlohmann::json enabled = Config();
  enabled["traceLogPath"] = "/tmp/run_js_trace.log";
  auto config = ParseConfig(enabled);
  ASSERT_TRUE(config.ok()) << config.status();
  EXPECT_EQ(config->trace_log_path, "/tmp/run_js_trace.log");
}

TEST(ConfigTest, RejectsInvalidTraceLogPaths) {
  std::string nul_path = "/tmp/run_js.log";
  nul_path.push_back('\0');
  nul_path.append("suffix");
  for (const nlohmann::json& path :
       {nlohmann::json("relative.log"), nlohmann::json(""), nlohmann::json(7), nlohmann::json(nullptr),
        nlohmann::json(std::string(4097, 'x')), nlohmann::json(nul_path)}) {
    nlohmann::json value = Config();
    value["traceLogPath"] = path;
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
  http["servers"][0]["transport"] = "unknown";
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
  mixed["servers"][0]["endpointUrl"] = "https://example.invalid";
  EXPECT_FALSE(ParseConfig(mixed).ok());

  nlohmann::json http = {{"servers",
                          {{{"alias", "docs"},
                            {"transport", "http"},
                            {"endpointUrl", "https://example.com/mcp"},
                            {"command", "/usr/bin/echo"},
                            {"args", nlohmann::json::array()}}}}};
  EXPECT_FALSE(ParseConfig(http).ok());

  nlohmann::json invalid_http = {{"servers", {{{"alias", "docs"}, {"transport", "http"}}}}};
  for (const nlohmann::json& url : {nlohmann::json(""),
                                    nlohmann::json("/mcp"),
                                    nlohmann::json("ftp://example.com"),
                                    nlohmann::json("https:///mcp"),
                                    nlohmann::json("https://user@example.com/mcp"),
                                    nlohmann::json("https://example.com/%ZZ"),
                                    nlohmann::json("https://example.com/?q=%"),
                                    nlohmann::json("http://:80/mcp"),
                                    nlohmann::json("http://example.com:/mcp"),
                                    nlohmann::json("http://example.com:abc/mcp"),
                                    nlohmann::json("http://example.com:65536/mcp"),
                                    nlohmann::json("http://127.000.0.1/mcp"),
                                    nlohmann::json("http://256.1.1.1/mcp"),
                                    nlohmann::json("http://[not-ipv6]/mcp"),
                                    nlohmann::json("http://[1:2:3:4:5:6:7:8:]/mcp"),
                                    nlohmann::json("http://[::ffff:192.0.2.001]/mcp"),
                                    nlohmann::json("http://[::1/mcp"),
                                    nlohmann::json("http://::1/mcp"),
                                    nlohmann::json("http://exa%mple.com/mcp"),
                                    nlohmann::json(std::string("https://example.com/\0mcp", 25)),
                                    nlohmann::json(std::string(4097, 'x'))}) {
    invalid_http["servers"][0]["endpointUrl"] = url;
    EXPECT_FALSE(ParseConfig(invalid_http).ok()) << url;
  }
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
