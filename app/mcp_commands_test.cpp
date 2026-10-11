#include "app/mcp_commands.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"

#include "core/http_client.h"
#include "mcp/client/registry.h"
#include "mcp/client/token_store.h"
#include "mcp/gateway/oauth_profile.h"

#include <gtest/gtest.h>

namespace slop {
namespace {

class FakeHttpClient : public HttpClient {
 public:
  absl::StatusOr<HttpResponse> PostWithResponse(const std::string& url, const std::string& body,
                                                const std::vector<std::string>& headers) override {
    post_url = url;
    post_body = body;
    post_headers = headers;
    return post_response;
  }

  absl::StatusOr<HttpResponse> PostOnceWithResponse(const std::string& url, const std::string& body,
                                                    const std::vector<std::string>& headers) override {
    return PostWithResponse(url, body, headers);
  }

  absl::StatusOr<std::string> GetOnce(const std::string& url, const std::vector<std::string>& headers) override {
    return Get(url, headers);
  }

  absl::StatusOr<std::string> Get(const std::string& url, const std::vector<std::string>& headers) override {
    get_urls.push_back(url);
    get_headers = headers;
    if (url == "https://api.example/.well-known/oauth-protected-resource") {
      return resource_metadata_body;
    }
    if (url == "https://auth.example/.well-known/oauth-authorization-server") {
      return authorization_metadata_body;
    }
    return absl::NotFoundError("unexpected URL");
  }

  HttpResponse post_response = {
      401,
      "",
      {{"www-authenticate", R"(Bearer resource_metadata="https://api.example/.well-known/oauth-protected-resource")"}}};
  std::string resource_metadata_body =
      R"({"resource":"https://api.example/mcp","authorization_servers":["https://auth.example"]})";
  std::string authorization_metadata_body =
      R"({"issuer":"https://auth.example","authorization_endpoint":"https://auth.example/authorize","token_endpoint":"https://auth.example/token","scopes_supported":["repo"],"code_challenge_methods_supported":["S256"]})";
  std::string post_url;
  std::string post_body;
  std::vector<std::string> post_headers;
  std::vector<std::string> get_urls;
  std::vector<std::string> get_headers;
};

class ScopedEnvironmentVariable {
 public:
  ScopedEnvironmentVariable(std::string name, std::string value) : name_(std::move(name)) {
    const char* previous = std::getenv(name_.c_str());
    if (previous != nullptr) previous_value_ = previous;
    set_result_ = setenv(name_.c_str(), value.c_str(), 1) == 0;
  }

  ~ScopedEnvironmentVariable() {
    if (previous_value_.has_value()) {
      setenv(name_.c_str(), previous_value_->c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  bool ok() const { return set_result_; }

 private:
  std::string name_;
  std::optional<std::string> previous_value_;
  bool set_result_ = false;
};

class ScopedHome {
 public:
  ScopedHome() {
    const char* home = std::getenv("HOME");
    if (home != nullptr) old_home_ = home;
    path_ = absl::StrCat(::testing::TempDir(), "/std_slop_mcp_commands_", absl::ToUnixNanos(absl::Now()));
    setenv("HOME", path_.c_str(), 1);
  }

  ~ScopedHome() {
    if (old_home_.empty()) {
      unsetenv("HOME");
    } else {
      setenv("HOME", old_home_.c_str(), 1);
    }
  }

 private:
  std::string old_home_;
  std::string path_;
};

TEST(McpCommandsTest, AddListAndRemoveRegistryEntry) {
  ScopedHome home;
  HttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status = RunMcpCommand({"mcp", "add", "github", "--url", "https://example.com/mcp", "--auth", "none"},
                                      &http_client, &input, &output, &error);
  ASSERT_TRUE(status.ok()) << status;
  EXPECT_NE(output.str().find("MCP server saved: github"), std::string::npos);

  auto entries = mcp::LoadServerRegistry(mcp::DefaultRegistryPath());
  ASSERT_TRUE(entries.ok()) << entries.status();
  ASSERT_EQ(entries->size(), 1);
  EXPECT_EQ((*entries)[0].name, "github");
  EXPECT_EQ((*entries)[0].url, "https://example.com/mcp");

  output.str("");
  output.clear();
  status = RunMcpCommand({"mcp", "list"}, &http_client, &input, &output, &error);
  ASSERT_TRUE(status.ok()) << status;
  EXPECT_NE(output.str().find("github\thttp\tnone\tenabled\thttps://example.com/mcp"), std::string::npos);

  output.str("");
  output.clear();
  status = RunMcpCommand({"mcp", "remove", "github"}, &http_client, &input, &output, &error);
  ASSERT_TRUE(status.ok()) << status;
  EXPECT_NE(output.str().find("MCP server removed: github"), std::string::npos);
  entries = mcp::LoadServerRegistry(mcp::DefaultRegistryPath());
  ASSERT_TRUE(entries.ok()) << entries.status();
  EXPECT_TRUE(entries->empty());
}

TEST(McpCommandsTest, AddsAndListsStdioWithoutStartingOrExposingArguments) {
  ScopedHome home;
  HttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;
  const std::vector<std::string> args = {"mcp",
                                         "add",
                                         "local",
                                         "--transport",
                                         "stdio",
                                         "--command",
                                         "/path/that/does/not/exist/server",
                                         "--args-json",
                                         R"(["two words", "$HOME", "#;", "--literal"] )"};

  const absl::Status status = RunMcpCommand(args, &http_client, &input, &output, &error);
  ASSERT_TRUE(status.ok()) << status;
  auto entries = mcp::LoadServerRegistry(mcp::DefaultRegistryPath());
  ASSERT_TRUE(entries.ok()) << entries.status();
  ASSERT_EQ(entries->size(), 1);
  EXPECT_EQ((*entries)[0].transport, mcp::kTransportStdio);
  EXPECT_EQ((*entries)[0].command, "/path/that/does/not/exist/server");
  EXPECT_EQ((*entries)[0].args, std::vector<std::string>({"two words", "$HOME", "#;", "--literal"}));

  output.str("");
  output.clear();
  ASSERT_TRUE(RunMcpCommand({"mcp", "list"}, &http_client, &input, &output, &error).ok());
  EXPECT_NE(output.str().find("local\tstdio\tnone\tenabled\t/path/that/does/not/exist/server"), std::string::npos);
  EXPECT_EQ(output.str().find("two words"), std::string::npos);
  EXPECT_EQ(output.str().find("$HOME"), std::string::npos);

  EXPECT_TRUE(RunMcpCommand({"mcp", "remove", "local"}, &http_client, &input, &output, &error).ok());
}

TEST(McpCommandsTest, DefaultsEmptyArgvAndRejectsIncompatibleOptions) {
  ScopedHome home;
  HttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  const absl::Status no_args_status =
      RunMcpCommand({"mcp", "add", "local", "--transport", "stdio", "--command", "/path/server"}, &http_client, &input,
                    &output, &error);
  ASSERT_TRUE(no_args_status.ok()) << no_args_status;
  auto entries = mcp::LoadServerRegistry(mcp::DefaultRegistryPath());
  ASSERT_TRUE(entries.ok()) << entries.status();
  ASSERT_EQ(entries->size(), 1);
  EXPECT_TRUE((*entries)[0].args.empty());
  EXPECT_FALSE(RunMcpCommand({"mcp", "add", "local", "--transport", "stdio", "--command", "/path/server", "--args-json",
                              "{", "--url", "https://example.com/mcp"},
                             &http_client, &input, &output, &error)
                   .ok());
  EXPECT_FALSE(RunMcpCommand({"mcp", "add", "local", "--command", "/path/server", "--args-json", "[]"}, &http_client,
                             &input, &output, &error)
                   .ok());
}

TEST(McpCommandsTest, AddRejectsDuplicateFlag) {
  ScopedHome home;
  HttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status =
      RunMcpCommand({"mcp", "add", "github", "--url", "https://one.example/mcp", "--url", "https://two.example/mcp"},
                    &http_client, &input, &output, &error);
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(status));
}

TEST(McpCommandsTest, AddBearerSavesTokenOutsideRegistry) {
  ScopedHome home;
  HttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status = RunMcpCommand(
      {"mcp", "add", "github", "--url", "https://example.com/mcp", "--auth", "bearer", "--token", " secret-token "},
      &http_client, &input, &output, &error);

  ASSERT_TRUE(status.ok()) << status;
  EXPECT_NE(output.str().find("MCP server saved: github"), std::string::npos);
  EXPECT_EQ(output.str().find("secret-token"), std::string::npos);

  auto entries = mcp::LoadServerRegistry(mcp::DefaultRegistryPath());
  ASSERT_TRUE(entries.ok()) << entries.status();
  ASSERT_EQ(entries->size(), 1);
  EXPECT_EQ((*entries)[0].auth, mcp::kAuthBearer);
  EXPECT_FALSE((*entries)[0].token_path.empty());

  auto tokens = mcp::LoadOAuthTokens((*entries)[0].token_path);
  ASSERT_TRUE(tokens.ok()) << tokens.status();
  EXPECT_EQ(tokens->access_token, "secret-token");

  std::ifstream registry_file(mcp::DefaultRegistryPath());
  const std::string registry_content((std::istreambuf_iterator<char>(registry_file)), std::istreambuf_iterator<char>());
  EXPECT_EQ(registry_content.find("secret-token"), std::string::npos);
}

TEST(McpCommandsTest, AddBearerRequiresToken) {
  ScopedHome home;
  HttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status = RunMcpCommand({"mcp", "add", "github", "--url", "https://example.com/mcp", "--auth", "bearer"},
                                      &http_client, &input, &output, &error);

  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(status));
  EXPECT_NE(std::string(status.message()).find("requires --token"), std::string::npos);
}

TEST(McpCommandsTest, AddRejectsTokenForNonBearerAuth) {
  ScopedHome home;
  HttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status = RunMcpCommand(
      {"mcp", "add", "github", "--url", "https://example.com/mcp", "--auth", "none", "--token", "secret-token"},
      &http_client, &input, &output, &error);

  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(status));
  EXPECT_NE(std::string(status.message()).find("only valid with --auth bearer"), std::string::npos);
}

TEST(McpCommandsTest, AddBearerDeletesOldTokenWhenTokenPathChanges) {
  ScopedHome home;
  HttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;
  const std::string old_path =
      absl::StrCat(::testing::TempDir(), "/old_mcp_bearer_", absl::ToUnixNanos(absl::Now()), ".json");
  const std::string new_path =
      absl::StrCat(::testing::TempDir(), "/new_mcp_bearer_", absl::ToUnixNanos(absl::Now()), ".json");

  ASSERT_TRUE(RunMcpCommand({"mcp", "add", "github", "--url", "https://example.com/mcp", "--auth", "bearer", "--token",
                             "old-token", "--token-path", old_path},
                            &http_client, &input, &output, &error)
                  .ok());
  ASSERT_TRUE(RunMcpCommand({"mcp", "add", "github", "--url", "https://example.com/mcp", "--auth", "bearer", "--token",
                             "new-token", "--token-path", new_path},
                            &http_client, &input, &output, &error)
                  .ok());

  EXPECT_FALSE(mcp::LoadOAuthTokens(old_path).ok());
  auto tokens = mcp::LoadOAuthTokens(new_path);
  ASSERT_TRUE(tokens.ok()) << tokens.status();
  EXPECT_EQ(tokens->access_token, "new-token");
}

TEST(McpCommandsTest, AddNonBearerDeletesOldBearerToken) {
  ScopedHome home;
  HttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;
  const std::string token_path =
      absl::StrCat(::testing::TempDir(), "/old_mcp_bearer_mode_", absl::ToUnixNanos(absl::Now()), ".json");

  ASSERT_TRUE(RunMcpCommand({"mcp", "add", "github", "--url", "https://example.com/mcp", "--auth", "bearer", "--token",
                             "old-token", "--token-path", token_path},
                            &http_client, &input, &output, &error)
                  .ok());
  ASSERT_TRUE(RunMcpCommand({"mcp", "add", "github", "--url", "https://example.com/mcp", "--auth", "none"},
                            &http_client, &input, &output, &error)
                  .ok());

  EXPECT_FALSE(mcp::LoadOAuthTokens(token_path).ok());
}

TEST(McpCommandsTest, AddBearerRestoresOldTokenWhenRegistryUpdateFails) {
  ScopedHome home;
  HttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;
  const std::string token_path =
      absl::StrCat(::testing::TempDir(), "/restore_mcp_bearer_", absl::ToUnixNanos(absl::Now()), ".json");

  ASSERT_TRUE(RunMcpCommand({"mcp", "add", "github", "--url", "https://example.com/mcp", "--auth", "bearer", "--token",
                             "old-token", "--token-path", token_path},
                            &http_client, &input, &output, &error)
                  .ok());

  const std::filesystem::path registry_dir = std::filesystem::path(mcp::DefaultRegistryPath()).parent_path();
  std::filesystem::permissions(registry_dir, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                               std::filesystem::perm_options::replace);
  const absl::Status status = RunMcpCommand({"mcp", "add", "github", "--url", "https://changed.example/mcp", "--auth",
                                             "bearer", "--token", "new-token", "--token-path", token_path},
                                            &http_client, &input, &output, &error);
  std::filesystem::permissions(
      registry_dir,
      std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec,
      std::filesystem::perm_options::replace);

  ASSERT_FALSE(status.ok());
  auto tokens = mcp::LoadOAuthTokens(token_path);
  ASSERT_TRUE(tokens.ok()) << tokens.status();
  EXPECT_EQ(tokens->access_token, "old-token");
}

TEST(McpCommandsTest, AddBearerDeletesNewTokenWhenRegistryUpdateFailsWithoutOldToken) {
  ScopedHome home;
  HttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;
  const std::string token_path =
      absl::StrCat(::testing::TempDir(), "/delete_new_mcp_bearer_", absl::ToUnixNanos(absl::Now()), ".json");

  ASSERT_TRUE(RunMcpCommand({"mcp", "add", "github", "--url", "https://example.com/mcp", "--auth", "bearer", "--token",
                             "old-token", "--token-path", token_path},
                            &http_client, &input, &output, &error)
                  .ok());
  ASSERT_TRUE(mcp::DeleteOAuthTokens(token_path).ok());

  const std::filesystem::path registry_dir = std::filesystem::path(mcp::DefaultRegistryPath()).parent_path();
  std::filesystem::permissions(registry_dir, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                               std::filesystem::perm_options::replace);
  const absl::Status status = RunMcpCommand({"mcp", "add", "github", "--url", "https://changed.example/mcp", "--auth",
                                             "bearer", "--token", "new-token", "--token-path", token_path},
                                            &http_client, &input, &output, &error);
  std::filesystem::permissions(
      registry_dir,
      std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec,
      std::filesystem::perm_options::replace);

  ASSERT_FALSE(status.ok());
  EXPECT_FALSE(mcp::LoadOAuthTokens(token_path).ok());
}

TEST(McpCommandsTest, OAuthAddDiscoversMissingEndpoints) {
  ScopedHome home;
  FakeHttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status = RunMcpCommand({"mcp", "add", "github", "--url", "https://api.example/mcp", "--auth", "oauth",
                                       "--client-id", "client", "--scope", "repo"},
                                      &http_client, &input, &output, &error);
  ASSERT_TRUE(status.ok()) << status;
  EXPECT_EQ(http_client.post_url, "https://api.example/mcp");
  ASSERT_EQ(http_client.get_urls.size(), 2);

  auto entries = mcp::LoadServerRegistry(mcp::DefaultRegistryPath());
  ASSERT_TRUE(entries.ok()) << entries.status();
  ASSERT_EQ(entries->size(), 1);
  EXPECT_EQ((*entries)[0].authorization_endpoint, "https://auth.example/authorize");
  EXPECT_EQ((*entries)[0].token_endpoint, "https://auth.example/token");
  EXPECT_EQ((*entries)[0].resource_metadata_url, "https://api.example/.well-known/oauth-protected-resource");
  EXPECT_EQ((*entries)[0].authorization_server_url, "https://auth.example");
  ASSERT_EQ((*entries)[0].scopes.size(), 1);
  EXPECT_EQ((*entries)[0].scopes[0], "repo");
}

TEST(McpCommandsTest, OAuthAddUsesIssuerToSelectAdvertisedAuthorizationServer) {
  ScopedHome home;
  FakeHttpClient http_client;
  http_client.resource_metadata_body =
      R"({"resource":"https://api.example/mcp","authorization_servers":["https://other.example","https://auth.example"]})";
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  const absl::Status status = RunMcpCommand({"mcp", "add", "github", "--url", "https://api.example/mcp", "--auth",
                                             "oauth", "--client-id", "client", "--issuer", "https://auth.example"},
                                            &http_client, &input, &output, &error);

  ASSERT_TRUE(status.ok()) << status;
  ASSERT_EQ(http_client.get_urls.size(), 2);
  EXPECT_EQ(http_client.get_urls[1], "https://auth.example/.well-known/oauth-authorization-server");
  auto entries = mcp::LoadServerRegistry(mcp::DefaultRegistryPath());
  ASSERT_TRUE(entries.ok()) << entries.status();
  ASSERT_EQ(entries->size(), 1);
  EXPECT_EQ((*entries)[0].authorization_server_url, "https://auth.example");
}

TEST(McpCommandsTest, OAuthAddUsesResourceMetadataUrlOverride) {
  ScopedHome home;
  FakeHttpClient http_client;
  http_client.post_response.headers.clear();
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  const absl::Status status =
      RunMcpCommand({"mcp", "add", "github", "--url", "https://api.example/mcp", "--auth", "oauth", "--client-id",
                     "client", "--resource-metadata-url", "https://api.example/.well-known/oauth-protected-resource"},
                    &http_client, &input, &output, &error);

  ASSERT_TRUE(status.ok()) << status;
  EXPECT_TRUE(http_client.post_url.empty());
  ASSERT_EQ(http_client.get_urls.size(), 2);
  EXPECT_EQ(http_client.get_urls[0], "https://api.example/.well-known/oauth-protected-resource");
  EXPECT_EQ(http_client.get_urls[1], "https://auth.example/.well-known/oauth-authorization-server");
}

TEST(McpCommandsTest, OAuthAddRejectsResourceMetadataOverrideWithManualEndpoints) {
  ScopedHome home;
  FakeHttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  const absl::Status status =
      RunMcpCommand({"mcp", "add", "github", "--url", "https://api.example/mcp", "--auth", "oauth", "--client-id",
                     "client", "--issuer", "https://manual.example", "--authorization-endpoint",
                     "https://manual.example/authorize", "--token-endpoint", "https://manual.example/token",
                     "--resource-metadata-url", "https://api.example/.well-known/oauth-protected-resource"},
                    &http_client, &input, &output, &error);

  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(status));
  EXPECT_TRUE(http_client.post_url.empty());
}

TEST(McpCommandsTest, OAuthAddManualEndpointsSkipsDiscovery) {
  ScopedHome home;
  FakeHttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status =
      RunMcpCommand({"mcp", "add", "github", "--url", "https://api.example/mcp", "--auth", "oauth", "--client-id",
                     "client", "--authorization-endpoint", "https://manual.example/authorize", "--token-endpoint",
                     "https://manual.example/token", "--issuer", "https://manual.example"},
                    &http_client, &input, &output, &error);
  ASSERT_TRUE(status.ok()) << status;
  EXPECT_TRUE(http_client.post_url.empty());

  auto entries = mcp::LoadServerRegistry(mcp::DefaultRegistryPath());
  ASSERT_TRUE(entries.ok()) << entries.status();
  ASSERT_EQ(entries->size(), 1);
  EXPECT_EQ((*entries)[0].authorization_endpoint, "https://manual.example/authorize");
  EXPECT_EQ((*entries)[0].token_endpoint, "https://manual.example/token");
}

TEST(McpCommandsTest, OAuthManualEndpointsRequireExplicitIssuer) {
  ScopedHome home;
  FakeHttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;
  const auto status = RunMcpCommand(
      {"mcp", "add", "manual", "--url", "https://api.example/mcp", "--auth", "oauth", "--client-id", "client",
       "--authorization-endpoint", "https://auth.example/authorize", "--token-endpoint", "https://auth.example/token"},
      &http_client, &input, &output, &error);
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_NE(std::string(status.message()).find("--issuer"), std::string::npos);
  EXPECT_TRUE(http_client.post_url.empty());
}

TEST(McpCommandsTest, OAuthAddRejectsPartialManualEndpoint) {
  ScopedHome home;
  FakeHttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status =
      RunMcpCommand({"mcp", "add", "github", "--url", "https://api.example/mcp", "--auth", "oauth", "--client-id",
                     "client", "--authorization-endpoint", "https://manual.example/authorize"},
                    &http_client, &input, &output, &error);
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(status));
  EXPECT_TRUE(http_client.post_url.empty());
}

TEST(McpCommandsTest, OAuthAddStillRequiresClientId) {
  ScopedHome home;
  FakeHttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status = RunMcpCommand({"mcp", "add", "github", "--url", "https://api.example/mcp", "--auth", "oauth"},
                                      &http_client, &input, &output, &error);
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(status));
}

TEST(McpCommandsTest, GatewayAndCoreDiscoveryConform) {
  ScopedHome home;
  FakeHttpClient http_client;
  http_client.post_response.status_code = 200;
  http_client.post_response.headers.clear();
  http_client.resource_metadata_body =
      R"({"resource":"https://api.example/mcp","authorization_servers":["https://other.example","https://auth.example"]})";

  mcp::gateway::ServerConfig gateway_server;
  gateway_server.alias = "github";
  gateway_server.transport = mcp::gateway::ServerTransport::kHttp;
  gateway_server.endpoint_url = "https://api.example/mcp";
  gateway_server.auth.method = mcp::HttpAuthMethod::kOAuth;
  gateway_server.auth.token_file = "/tmp/gateway-conformance-token.json";
  gateway_server.auth.client_id = "client";
  gateway_server.auth.expected_issuer = "https://auth.example";
  gateway_server.auth.scopes = {"repo"};

  const auto gateway_auth = mcp::gateway::ResolveOAuthAuthConfig(gateway_server, &http_client);

  ASSERT_TRUE(gateway_auth.ok()) << gateway_auth.status();
  const std::vector<std::string> gateway_metadata_requests = http_client.get_urls;
  EXPECT_EQ(gateway_metadata_requests,
            (std::vector<std::string>{"https://api.example/.well-known/oauth-protected-resource/mcp",
                                      "https://api.example/.well-known/oauth-protected-resource",
                                      "https://auth.example/.well-known/oauth-authorization-server"}));

  http_client.post_url.clear();
  http_client.get_urls.clear();
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;
  const absl::Status core_status =
      RunMcpCommand({"mcp", "add", "github", "--url", "https://api.example/mcp", "--auth", "oauth", "--client-id",
                     "client", "--issuer", "https://auth.example", "--scope", "repo"},
                    &http_client, &input, &output, &error);

  ASSERT_TRUE(core_status.ok()) << core_status;
  EXPECT_EQ(http_client.get_urls, gateway_metadata_requests);
  auto entries = mcp::LoadServerRegistry(mcp::DefaultRegistryPath());
  ASSERT_TRUE(entries.ok()) << entries.status();
  ASSERT_EQ(entries->size(), 1);
  const mcp::ServerRegistryEntry& core_entry = entries->front();
  EXPECT_EQ(core_entry.authorization_server_url, gateway_auth->expected_issuer);
  EXPECT_EQ(core_entry.resource_metadata_url, gateway_auth->resource_metadata_url);
  EXPECT_EQ(core_entry.authorization_endpoint, gateway_auth->authorization_endpoint);
  EXPECT_EQ(core_entry.token_endpoint, gateway_auth->token_endpoint);
  EXPECT_EQ(core_entry.scopes, gateway_auth->scopes);
}

TEST(McpCommandsTest, OAuthAddFallsBackToWellKnownMetadata) {
  ScopedHome home;
  FakeHttpClient http_client;
  http_client.post_response.headers.clear();
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  const absl::Status status = RunMcpCommand(
      {"mcp", "add", "github", "--url", "https://api.example/mcp", "--auth", "oauth", "--client-id", "client"},
      &http_client, &input, &output, &error);

  ASSERT_TRUE(status.ok()) << status;
  ASSERT_EQ(http_client.get_urls.size(), 3);
  EXPECT_EQ(http_client.get_urls[0], "https://api.example/.well-known/oauth-protected-resource/mcp");
  EXPECT_EQ(http_client.get_urls[1], "https://api.example/.well-known/oauth-protected-resource");
  EXPECT_EQ(http_client.get_urls[2], "https://auth.example/.well-known/oauth-authorization-server");
}

TEST(McpCommandsTest, HelpCommandPrintsPrescriptiveUsage) {
  FakeHttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status = RunMcpCommand({"mcp", "help"}, &http_client, &input, &output, &error);
  ASSERT_TRUE(status.ok()) << status;
  EXPECT_NE(output.str().find("std_slop mcp add githubcopilot"), std::string::npos);
  EXPECT_NE(output.str().find("--transport stdio --command"), std::string::npos);
  EXPECT_NE(output.str().find("Arguments are passed literally, without a shell"), std::string::npos);
}

TEST(McpCommandsTest, WrongArgumentsReturnPrescriptiveUsage) {
  FakeHttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status = RunMcpCommand({"mcp", "add"}, &http_client, &input, &output, &error);
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(status));
  EXPECT_NE(std::string(status.message()).find("OAuth endpoints are discovered"), std::string::npos);
  EXPECT_NE(std::string(status.message()).find("--auth bearer --token <token>"), std::string::npos);
  EXPECT_NE(std::string(status.message()).find("oauth-login <name>"), std::string::npos);
  EXPECT_NE(std::string(status.message()).find("oauth-refresh <name>"), std::string::npos);
}

TEST(McpCommandsTest, OAuthLoginUsesRenamedCommand) {
  ScopedHome home;
  FakeHttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status = RunMcpCommand({"mcp", "oauth-login", "missing"}, &http_client, &input, &output, &error);
  EXPECT_TRUE(absl::IsNotFound(status));

  status = RunMcpCommand({"mcp", "login", "missing"}, &http_client, &input, &output, &error);
  EXPECT_TRUE(absl::IsInvalidArgument(status));
  EXPECT_NE(std::string(status.message()).find("oauth-login <name>"), std::string::npos);
}

TEST(McpCommandsTest, RefreshAddsServerContextToTokenErrors) {
  ScopedHome home;
  FakeHttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status =
      RunMcpCommand({"mcp", "add", "github", "--url", "https://api.example/mcp", "--auth", "oauth", "--client-id",
                     "client", "--authorization-endpoint", "https://manual.example/authorize", "--token-endpoint",
                     "https://manual.example/token", "--issuer", "https://manual.example"},
                    &http_client, &input, &output, &error);
  ASSERT_TRUE(status.ok()) << status;

  status = RunMcpCommand({"mcp", "oauth-refresh", "github"}, &http_client, &input, &output, &error);
  EXPECT_FALSE(status.ok());
  EXPECT_NE(std::string(status.message()).find("MCP token refresh failed for server 'github'"), std::string::npos);
}

TEST(McpCommandsTest, RefreshUsesClientSecretEnvironmentWithoutPersistingIt) {
  ScopedHome home;
  ScopedEnvironmentVariable secret("SLOP_MCP_CLIENT_SECRET", "top secret");
  ASSERT_TRUE(secret.ok());
  FakeHttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status =
      RunMcpCommand({"mcp", "add", "github", "--url", "https://api.example/mcp", "--auth", "oauth", "--client-id",
                     "client", "--authorization-endpoint", "https://manual.example/authorize", "--token-endpoint",
                     "https://manual.example/token", "--issuer", "https://manual.example"},
                    &http_client, &input, &output, &error);
  ASSERT_TRUE(status.ok()) << status;
  mcp::OAuthTokenSet old_tokens;
  old_tokens.access_token = "old-access";
  old_tokens.refresh_token = "old-refresh";
  old_tokens.issuer = "https://manual.example";
  old_tokens.resource = "https://api.example/mcp";
  ASSERT_TRUE(mcp::SaveOAuthTokens(mcp::DefaultTokenPath("github"), old_tokens).ok());

  http_client.post_response = {
      200, R"({"access_token":"new-access","refresh_token":"new-refresh","token_type":"Bearer","expires_in":60})", {}};
  status = RunMcpCommand({"mcp", "oauth-refresh", "github"}, &http_client, &input, &output, &error);
  ASSERT_TRUE(status.ok()) << status;
  EXPECT_NE(http_client.post_body.find("client_secret=top%20secret"), std::string::npos);

  auto tokens = mcp::LoadOAuthTokens(mcp::DefaultTokenPath("github"));
  ASSERT_TRUE(tokens.ok()) << tokens.status();
  EXPECT_EQ(tokens->access_token, "new-access");
  EXPECT_EQ(output.str().find("top secret"), std::string::npos);
}

TEST(McpCommandsTest, OAuthCommandsRejectClientSecretArguments) {
  ScopedHome home;
  FakeHttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  const absl::Status status = RunMcpCommand({"mcp", "oauth-login", "github", "--client-secret", "argv-secret"},
                                            &http_client, &input, &output, &error);
  EXPECT_TRUE(absl::IsInvalidArgument(status));
  EXPECT_EQ(std::string(status.message()).find("argv-secret"), std::string::npos);
}

TEST(McpCommandsTest, AddRejectsClientSecretBecauseItIsNotPersisted) {
  ScopedHome home;
  FakeHttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status = RunMcpCommand({"mcp", "add", "github", "--url", "https://api.example/mcp", "--auth", "oauth",
                                       "--client-id", "client", "--client-secret", "top secret"},
                                      &http_client, &input, &output, &error);
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(status));
  EXPECT_NE(std::string(status.message()).find("Unknown MCP flag: --client-secret"), std::string::npos);
}

}  // namespace
}  // namespace slop
