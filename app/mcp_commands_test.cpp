#include "app/mcp_commands.h"

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "core/http_client.h"
#include "mcp/registry.h"

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

  HttpResponse post_response = {401, "", {{"www-authenticate", R"(Bearer resource_metadata="https://api.example/.well-known/oauth-protected-resource")"}}};
  std::string resource_metadata_body =
      R"({"resource":"https://api.example/mcp","authorization_servers":["https://auth.example"]})";
  std::string authorization_metadata_body =
      R"({"issuer":"https://auth.example","authorization_endpoint":"https://auth.example/authorize","token_endpoint":"https://auth.example/token","scopes_supported":["repo"]})";
  std::string post_url;
  std::string post_body;
  std::vector<std::string> post_headers;
  std::vector<std::string> get_urls;
  std::vector<std::string> get_headers;
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
  EXPECT_NE(output.str().find("github\tnone\tenabled\thttps://example.com/mcp"), std::string::npos);

  output.str("");
  output.clear();
  status = RunMcpCommand({"mcp", "remove", "github"}, &http_client, &input, &output, &error);
  ASSERT_TRUE(status.ok()) << status;
  EXPECT_NE(output.str().find("MCP server removed: github"), std::string::npos);
  entries = mcp::LoadServerRegistry(mcp::DefaultRegistryPath());
  ASSERT_TRUE(entries.ok()) << entries.status();
  EXPECT_TRUE(entries->empty());
}

TEST(McpCommandsTest, AddRejectsDuplicateFlag) {
  ScopedHome home;
  HttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status = RunMcpCommand({"mcp", "add", "github", "--url", "https://one.example/mcp", "--url",
                                       "https://two.example/mcp"},
                                      &http_client, &input, &output, &error);
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(status));
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

TEST(McpCommandsTest, OAuthAddManualEndpointsSkipsDiscovery) {
  ScopedHome home;
  FakeHttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status = RunMcpCommand({"mcp", "add", "github", "--url", "https://api.example/mcp", "--auth", "oauth",
                                       "--client-id", "client", "--authorization-endpoint",
                                       "https://manual.example/authorize", "--token-endpoint",
                                       "https://manual.example/token"},
                                      &http_client, &input, &output, &error);
  ASSERT_TRUE(status.ok()) << status;
  EXPECT_TRUE(http_client.post_url.empty());

  auto entries = mcp::LoadServerRegistry(mcp::DefaultRegistryPath());
  ASSERT_TRUE(entries.ok()) << entries.status();
  ASSERT_EQ(entries->size(), 1);
  EXPECT_EQ((*entries)[0].authorization_endpoint, "https://manual.example/authorize");
  EXPECT_EQ((*entries)[0].token_endpoint, "https://manual.example/token");
}

TEST(McpCommandsTest, OAuthAddRejectsPartialManualEndpoint) {
  ScopedHome home;
  FakeHttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status = RunMcpCommand({"mcp", "add", "github", "--url", "https://api.example/mcp", "--auth", "oauth",
                                       "--client-id", "client", "--authorization-endpoint",
                                       "https://manual.example/authorize"},
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

TEST(McpCommandsTest, OAuthAddReportsDiscoveryFailure) {
  ScopedHome home;
  FakeHttpClient http_client;
  http_client.post_response.headers.clear();
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status = RunMcpCommand({"mcp", "add", "github", "--url", "https://api.example/mcp", "--auth", "oauth",
                                       "--client-id", "client"},
                                      &http_client, &input, &output, &error);
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(absl::IsUnauthenticated(status));
}

TEST(McpCommandsTest, HelpCommandPrintsPrescriptiveUsage) {
  FakeHttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status = RunMcpCommand({"mcp", "help"}, &http_client, &input, &output, &error);
  ASSERT_TRUE(status.ok()) << status;
  EXPECT_NE(output.str().find("std_slop mcp add githubcopilot"), std::string::npos);
  EXPECT_NE(output.str().find("registered OAuth/GitHub App"), std::string::npos);
}

TEST(McpCommandsTest, WrongArgumentsReturnPrescriptiveUsage) {
  FakeHttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status = RunMcpCommand({"mcp", "add"}, &http_client, &input, &output, &error);
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(status));
  EXPECT_NE(std::string(status.message()).find("OAuth endpoints are discovered when both are omitted"),
            std::string::npos);
}

}  // namespace
}  // namespace slop
