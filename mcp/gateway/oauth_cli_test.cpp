#include "mcp/gateway/oauth_cli.h"

#include <cstdlib>
#include <filesystem>
#include <istream>
#include <optional>
#include <ostream>
#include <streambuf>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"

#include "mcp/client/token_store.h"

namespace slop::mcp::gateway {
namespace {

class FakeHttpClient : public slop::HttpClient {
 public:
  absl::StatusOr<slop::HttpResponse> PostOnceWithResponse(const std::string& url, const std::string& body,
                                                          const std::vector<std::string>& headers) override {
    last_url = url;
    last_body = body;
    last_headers = headers;
    return response;
  }

  slop::HttpResponse response{
      200,
      R"({"access_token":"access-token","refresh_token":"refresh-token","token_type":"Bearer","expires_in":60})",
      {}};
  std::string last_url;
  std::string last_body;
  std::vector<std::string> last_headers;
};

class DiscoveryHttpClient : public slop::HttpClient {
 public:
  absl::StatusOr<slop::HttpResponse> PostOnceWithResponse(const std::string& url, const std::string&,
                                                          const std::vector<std::string>&) override {
    if (url == "https://resource.example/mcp") {
      return slop::HttpResponse{
          401,
          "",
          {{"www-authenticate",
            R"(Bearer resource_metadata="https://resource.example/.well-known/oauth-protected-resource")"}}};
    }
    last_token_url = url;
    return token_response;
  }

  absl::StatusOr<std::string> GetOnce(const std::string& url, const std::vector<std::string>&) override {
    get_urls.push_back(url);
    if (url == resource_metadata_url) {
      return R"({"resource":"https://resource.example/mcp","authorization_servers":["https://issuer.example"]})";
    }
    if (url == authorization_metadata_url) {
      return R"({"issuer":"https://issuer.example","authorization_endpoint":"https://issuer.example/authorize","token_endpoint":"https://issuer.example/token","code_challenge_methods_supported":["S256"]})";
    }
    return absl::NotFoundError("unexpected metadata URL");
  }

  const std::string resource_metadata_url = "https://resource.example/.well-known/oauth-protected-resource";
  const std::string authorization_metadata_url = "https://issuer.example/.well-known/oauth-authorization-server";
  slop::HttpResponse token_response{
      200,
      R"({"access_token":"access-token","refresh_token":"refresh-token","token_type":"Bearer","expires_in":60})",
      {}};
  std::string last_token_url;
  std::vector<std::string> get_urls;
};

class ScopedEnvironmentVariable {
 public:
  explicit ScopedEnvironmentVariable(std::string name) : name_(std::move(name)) {
    const char* previous = std::getenv(name_.c_str());
    if (previous != nullptr) previous_value_ = previous;
    set_result_ = unsetenv(name_.c_str()) == 0;
  }

  ScopedEnvironmentVariable(std::string name, std::string value) : name_(std::move(name)) {
    const char* previous = std::getenv(name_.c_str());
    if (previous != nullptr) previous_value_ = previous;
    set_result_ = setenv(name_.c_str(), value.c_str(), 1) == 0;
  }

  bool ok() const { return set_result_; }

  ~ScopedEnvironmentVariable() {
    if (previous_value_.has_value()) {
      setenv(name_.c_str(), previous_value_->c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

 private:
  std::string name_;
  std::optional<std::string> previous_value_;
  bool set_result_ = false;
};

class CallbackInputBuffer : public std::streambuf {
 public:
  explicit CallbackInputBuffer(const std::ostringstream* output) : output_(output) {}

 protected:
  int_type underflow() override {
    if (!initialized_) {
      initialized_ = true;
      const std::string output = output_->str();
      const std::size_t state_start = output.find("state=");
      if (state_start == std::string::npos) return traits_type::eof();
      const std::size_t value_start = state_start + 6;
      const std::size_t value_end = output.find('&', value_start);
      if (value_end == std::string::npos) return traits_type::eof();
      callback_ = absl::StrCat("http://127.0.0.1/callback?code=authorization-code&state=",
                               output.substr(value_start, value_end - value_start), "\n");
      setg(callback_.data(), callback_.data(), callback_.data() + callback_.size());
    }
    if (gptr() == egptr()) return traits_type::eof();
    return traits_type::to_int_type(*gptr());
  }

 private:
  const std::ostringstream* output_;
  std::string callback_;
  bool initialized_ = false;
};

std::string TestTokenPath(const std::string& name) { return (std::filesystem::temp_directory_path() / name).string(); }

GatewayConfig OAuthConfig(const std::string& token_file) {
  GatewayConfig config;
  ServerConfig server;
  server.alias = "docs";
  server.transport = ServerTransport::kHttp;
  server.endpoint_url = "https://resource.example/mcp";
  server.auth.method = HttpAuthMethod::kOAuth;
  server.auth.token_file = token_file;
  server.auth.client_id = "docs-client";
  server.auth.client_secret_env = "SLOP_TEST_GATEWAY_OAUTH_SECRET";
  server.auth.expected_issuer = "https://issuer.example";
  server.auth.authorization_endpoint = "https://issuer.example/authorize";
  server.auth.token_endpoint = "https://issuer.example/token";
  server.auth.scopes = {"read", "write"};
  config.servers.push_back(std::move(server));
  return config;
}

TEST(OAuthCliTest, ParsesCommandsAndRejectsSecretArguments) {
  auto parsed = ParseOAuthCliOptions({"auth", "login", "--config", "gateway.json", "--server", "docs"});
  ASSERT_TRUE(parsed.ok()) << parsed.status();
  EXPECT_EQ(parsed->action, OAuthCliAction::kLogin);
  EXPECT_EQ(parsed->config_path, "gateway.json");
  EXPECT_EQ(parsed->server_alias, "docs");

  parsed = ParseOAuthCliOptions({"auth", "refresh", "--config", "gateway.json", "--server", "docs"});
  ASSERT_TRUE(parsed.ok()) << parsed.status();
  EXPECT_EQ(parsed->action, OAuthCliAction::kRefresh);

  EXPECT_FALSE(ParseOAuthCliOptions(
                   {"auth", "login", "--config", "gateway.json", "--server", "docs", "--client-secret", "secret"})
                   .ok());
  EXPECT_FALSE(
      ParseOAuthCliOptions({"auth", "login", "--config", "gateway.json", "--config", "other.json", "--server", "docs"})
          .ok());
  EXPECT_FALSE(ParseOAuthCliOptions({"auth", "delete", "--config", "gateway.json", "--server", "docs"}).ok());
  auto invalid_alias = ParseOAuthCliOptions({"auth", "login", "--config", "gateway.json", "--server", "docs\n"});
  ASSERT_FALSE(invalid_alias.ok());
  EXPECT_EQ(invalid_alias.status().message().find('\n'), std::string::npos);
}

TEST(OAuthCliTest, RequiresConfiguredClientSecretEnvironmentValue) {
  ScopedEnvironmentVariable secret("SLOP_TEST_GATEWAY_OAUTH_SECRET");
  ASSERT_TRUE(secret.ok());
  FakeHttpClient http;
  std::ostringstream output;
  const OAuthCliOptions options{OAuthCliAction::kRefresh, "gateway.json", "docs"};

  const absl::Status status = RunOAuthCliCommand(options, OAuthConfig("/tmp/token.json"), &http, nullptr, &output);

  EXPECT_TRUE(absl::IsFailedPrecondition(status));
  EXPECT_TRUE(http.last_url.empty());
  EXPECT_TRUE(output.str().empty());
}

TEST(OAuthCliTest, LoginUsesPkceAndDoesNotPrintSecretsOrTokens) {
  const std::string token_file = TestTokenPath("slop_gateway_oauth_cli_login_test.json");
  std::filesystem::remove(token_file);
  ScopedEnvironmentVariable secret("SLOP_TEST_GATEWAY_OAUTH_SECRET", "top secret");
  ASSERT_TRUE(secret.ok());
  FakeHttpClient http;
  const OAuthCliOptions options{OAuthCliAction::kLogin, "gateway.json", "docs"};
  const GatewayConfig config = OAuthConfig(token_file);
  std::ostringstream output;
  CallbackInputBuffer input_buffer(&output);
  std::istream input(&input_buffer);

  const absl::Status status = RunOAuthCliCommand(options, config, &http, &input, &output);

  ASSERT_TRUE(status.ok()) << status;
  auto tokens = slop::mcp::LoadOAuthTokens(token_file);
  ASSERT_TRUE(tokens.ok()) << tokens.status();
  EXPECT_EQ(tokens->access_token, "access-token");
  EXPECT_EQ(tokens->refresh_token, "refresh-token");
  EXPECT_EQ(tokens->issuer, "https://issuer.example");
  EXPECT_EQ(tokens->resource, "https://resource.example/mcp");
  EXPECT_NE(http.last_body.find("client_secret=top%20secret"), std::string::npos);
  EXPECT_EQ(output.str().find("top secret"), std::string::npos);
  EXPECT_EQ(output.str().find("access-token"), std::string::npos);
  EXPECT_EQ(output.str().find("refresh-token"), std::string::npos);
  EXPECT_NE(output.str().find("code_challenge_method=S256"), std::string::npos);
  std::filesystem::remove(token_file);
}

TEST(OAuthCliTest, LoginDiscoversOAuthEndpointsFromWellKnownMetadata) {
  const std::string token_file = TestTokenPath("slop_gateway_oauth_cli_discovery_login_test.json");
  std::filesystem::remove(token_file);
  DiscoveryHttpClient http;
  const OAuthCliOptions options{OAuthCliAction::kLogin, "gateway.json", "docs"};
  GatewayConfig config = OAuthConfig(token_file);
  HttpAuthConfig& auth = config.servers[0].auth;
  auth.client_secret_env.clear();
  auth.expected_issuer.clear();
  auth.authorization_endpoint.clear();
  auth.token_endpoint.clear();
  auth.resource_metadata_url.clear();
  std::ostringstream output;
  CallbackInputBuffer input_buffer(&output);
  std::istream input(&input_buffer);

  const absl::Status status = RunOAuthCliCommand(options, config, &http, &input, &output);

  ASSERT_TRUE(status.ok()) << status;
  ASSERT_EQ(http.get_urls.size(), 2);
  EXPECT_EQ(http.get_urls[0], http.resource_metadata_url);
  EXPECT_EQ(http.get_urls[1], http.authorization_metadata_url);
  EXPECT_EQ(http.last_token_url, "https://issuer.example/token");
  auto tokens = slop::mcp::LoadOAuthTokens(token_file);
  ASSERT_TRUE(tokens.ok()) << tokens.status();
  EXPECT_EQ(tokens->issuer, "https://issuer.example");
  EXPECT_EQ(tokens->resource, "https://resource.example/mcp");
  std::filesystem::remove(token_file);
}

TEST(OAuthCliTest, RefreshDiscoversOAuthEndpointsFromWellKnownMetadata) {
  const std::string token_file = TestTokenPath("slop_gateway_oauth_cli_discovery_refresh_test.json");
  std::filesystem::remove(token_file);
  slop::mcp::OAuthTokenSet old_tokens;
  old_tokens.access_token = "old-access-token";
  old_tokens.refresh_token = "old-refresh-token";
  old_tokens.issuer = "https://issuer.example";
  old_tokens.resource = "https://resource.example/mcp";
  ASSERT_TRUE(slop::mcp::SaveOAuthTokens(token_file, old_tokens).ok());
  DiscoveryHttpClient http;
  const OAuthCliOptions options{OAuthCliAction::kRefresh, "gateway.json", "docs"};
  GatewayConfig config = OAuthConfig(token_file);
  HttpAuthConfig& auth = config.servers[0].auth;
  auth.client_secret_env.clear();
  auth.expected_issuer.clear();
  auth.authorization_endpoint.clear();
  auth.token_endpoint.clear();
  auth.resource_metadata_url.clear();
  std::ostringstream output;

  const absl::Status status = RunOAuthCliCommand(options, config, &http, nullptr, &output);

  ASSERT_TRUE(status.ok()) << status;
  ASSERT_EQ(http.get_urls.size(), 2);
  EXPECT_EQ(http.get_urls[1], http.authorization_metadata_url);
  EXPECT_EQ(http.last_token_url, "https://issuer.example/token");
  auto tokens = slop::mcp::LoadOAuthTokens(token_file);
  ASSERT_TRUE(tokens.ok()) << tokens.status();
  EXPECT_EQ(tokens->access_token, "access-token");
  EXPECT_EQ(tokens->issuer, "https://issuer.example");
  EXPECT_EQ(tokens->resource, "https://resource.example/mcp");
  std::filesystem::remove(token_file);
}

TEST(OAuthCliTest, RefreshUsesStoredRefreshTokenAndPreservesIt) {
  const std::string token_file = TestTokenPath("slop_gateway_oauth_cli_refresh_test.json");
  std::filesystem::remove(token_file);
  ScopedEnvironmentVariable secret("SLOP_TEST_GATEWAY_OAUTH_SECRET", "refresh secret");
  ASSERT_TRUE(secret.ok());
  slop::mcp::OAuthTokenSet previous_tokens;
  previous_tokens.access_token = "previous-access";
  previous_tokens.refresh_token = "previous-refresh";
  previous_tokens.issuer = "https://issuer.example";
  previous_tokens.resource = "https://resource.example/mcp";
  ASSERT_TRUE(slop::mcp::SaveOAuthTokens(token_file, previous_tokens).ok());

  FakeHttpClient http;
  http.response.body = R"({"access_token":"next-access","token_type":"Bearer","expires_in":60})";
  const OAuthCliOptions options{OAuthCliAction::kRefresh, "gateway.json", "docs"};
  const GatewayConfig config = OAuthConfig(token_file);
  std::ostringstream output;

  const absl::Status status = RunOAuthCliCommand(options, config, &http, nullptr, &output);

  ASSERT_TRUE(status.ok()) << status;
  auto tokens = slop::mcp::LoadOAuthTokens(token_file);
  ASSERT_TRUE(tokens.ok()) << tokens.status();
  EXPECT_EQ(tokens->access_token, "next-access");
  EXPECT_EQ(tokens->refresh_token, "previous-refresh");
  EXPECT_NE(http.last_body.find("refresh_token=previous-refresh"), std::string::npos);
  EXPECT_NE(http.last_body.find("client_secret=refresh%20secret"), std::string::npos);
  EXPECT_EQ(output.str().find("next-access"), std::string::npos);
  EXPECT_EQ(output.str().find("previous-refresh"), std::string::npos);
  std::filesystem::remove(token_file);
}

TEST(OAuthCliTest, RejectsMissingServerAndNonOauthProfiles) {
  FakeHttpClient http;
  std::ostringstream output;
  const OAuthCliOptions options{OAuthCliAction::kRefresh, "gateway.json", "unknown"};
  EXPECT_TRUE(absl::IsNotFound(RunOAuthCliCommand(options, OAuthConfig("/tmp/token.json"), &http, nullptr, &output)));

  GatewayConfig config = OAuthConfig("/tmp/token.json");
  config.servers[0].auth.method = HttpAuthMethod::kBearer;
  config.servers[0].auth.token_env = "TOKEN";
  const OAuthCliOptions bearer_options{OAuthCliAction::kRefresh, "gateway.json", "docs"};
  EXPECT_TRUE(absl::IsInvalidArgument(RunOAuthCliCommand(bearer_options, config, &http, nullptr, &output)));
}

}  // namespace
}  // namespace slop::mcp::gateway
