#include "mcp/gateway/oauth_profile.h"

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "gtest/gtest.h"

namespace slop::mcp::gateway {
namespace {

class DiscoveryHttpClient : public ::slop::HttpClient {
 public:
  absl::StatusOr<::slop::HttpResponse> PostOnceWithResponse(const std::string& url, const std::string&,
                                                            const std::vector<std::string>&) override {
    post_url = url;
    return challenge;
  }

  absl::StatusOr<std::string> GetOnce(const std::string& url, const std::vector<std::string>&) override {
    get_urls.push_back(url);
    if (url == resource_metadata_url) return resource_metadata_body;
    if (url.find("/.well-known/oauth-authorization-server") != std::string::npos) {
      return authorization_metadata_body;
    }
    return absl::NotFoundError("unexpected metadata URL");
  }

  ::slop::HttpResponse challenge{
      401,
      "",
      {{"www-authenticate", R"(Bearer resource_metadata="https://api.example/.well-known/oauth-protected-resource")"}}};
  const std::string resource_metadata_url = "https://api.example/.well-known/oauth-protected-resource";
  const std::string authorization_metadata_url = "https://auth.example/.well-known/oauth-authorization-server";
  std::string resource_metadata_body =
      R"({"resource":"https://api.example/mcp","authorization_servers":["https://auth.example"]})";
  const std::string authorization_metadata_body =
      R"({"issuer":"https://auth.example","authorization_endpoint":"https://auth.example/authorize","token_endpoint":"https://auth.example/token","code_challenge_methods_supported":["S256"]})";
  std::string post_url;
  std::vector<std::string> get_urls;
};

ServerConfig MinimalOAuthServer() {
  ServerConfig server;
  server.alias = "docs";
  server.transport = ServerTransport::kHttp;
  server.endpoint_url = "https://api.example/mcp";
  server.auth.method = HttpAuthMethod::kOAuth;
  server.auth.token_file = "/tmp/docs-token.json";
  server.auth.client_id = "docs-client";
  return server;
}

TEST(OAuthProfileTest, DiscoversIssuerAndEndpointsFromProtectedResourceMetadata) {
  DiscoveryHttpClient http;

  auto auth = ResolveOAuthAuthConfig(MinimalOAuthServer(), &http);

  ASSERT_TRUE(auth.ok()) << auth.status();
  EXPECT_EQ(http.post_url, "https://api.example/mcp");
  ASSERT_EQ(http.get_urls.size(), 2);
  EXPECT_EQ(http.get_urls[0], http.resource_metadata_url);
  EXPECT_EQ(http.get_urls[1], http.authorization_metadata_url);
  EXPECT_EQ(auth->expected_issuer, "https://auth.example");
  EXPECT_EQ(auth->authorization_endpoint, "https://auth.example/authorize");
  EXPECT_EQ(auth->token_endpoint, "https://auth.example/token");
}

TEST(OAuthProfileTest, UsesConfiguredIssuerToSelectAuthorizationServer) {
  DiscoveryHttpClient http;
  http.resource_metadata_body =
      R"({"resource":"https://api.example/mcp","authorization_servers":["https://other.example","https://auth.example"]})";
  ServerConfig server = MinimalOAuthServer();
  server.auth.expected_issuer = "https://auth.example";

  auto auth = ResolveOAuthAuthConfig(server, &http);

  ASSERT_TRUE(auth.ok()) << auth.status();
  EXPECT_EQ(auth->expected_issuer, server.auth.expected_issuer);
}

TEST(OAuthProfileTest, RejectsAmbiguousAuthorizationServersWithoutIssuer) {
  DiscoveryHttpClient http;
  http.resource_metadata_body =
      R"({"resource":"https://api.example/mcp","authorization_servers":["https://other.example","https://auth.example"]})";

  auto auth = ResolveOAuthAuthConfig(MinimalOAuthServer(), &http);

  ASSERT_FALSE(auth.ok());
  EXPECT_TRUE(absl::IsFailedPrecondition(auth.status()));
  EXPECT_EQ(http.get_urls.size(), 1);
}

TEST(OAuthProfileTest, RejectsIssuerNotAdvertisedByResourceMetadata) {
  DiscoveryHttpClient http;
  ServerConfig server = MinimalOAuthServer();
  server.auth.expected_issuer = "https://unexpected.example";

  auto auth = ResolveOAuthAuthConfig(server, &http);

  ASSERT_FALSE(auth.ok());
  EXPECT_TRUE(absl::IsPermissionDenied(auth.status()));
}

TEST(OAuthProfileTest, UsesConfiguredResourceMetadataUrlWithoutProbe) {
  DiscoveryHttpClient http;
  ServerConfig server = MinimalOAuthServer();
  server.auth.resource_metadata_url = http.resource_metadata_url;

  auto auth = ResolveOAuthAuthConfig(server, &http);

  ASSERT_TRUE(auth.ok()) << auth.status();
  EXPECT_TRUE(http.post_url.empty());
  ASSERT_EQ(http.get_urls.size(), 2);
  EXPECT_EQ(http.get_urls[0], http.resource_metadata_url);
}

}  // namespace
}  // namespace slop::mcp::gateway
