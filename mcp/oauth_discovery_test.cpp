#include "mcp/oauth_discovery.h"

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "core/http_client.h"

#include <gtest/gtest.h>

namespace slop::mcp {
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
    if (url == resource_metadata_url) return resource_metadata_body;
    if (url == authorization_metadata_url) return authorization_metadata_body;
    return absl::NotFoundError("unexpected URL");
  }

  HttpResponse post_response = {401, "", {{"www-authenticate", R"(Bearer resource_metadata="https://api.example/.well-known/oauth-protected-resource")"}}};
  std::string resource_metadata_url = "https://api.example/.well-known/oauth-protected-resource";
  std::string authorization_metadata_url = "https://auth.example/.well-known/oauth-authorization-server";
  std::string resource_metadata_body =
      R"({"resource":"https://api.example/mcp","authorization_servers":["https://auth.example"]})";
  std::string authorization_metadata_body =
      R"({"issuer":"https://auth.example","authorization_endpoint":"https://auth.example/authorize","token_endpoint":"https://auth.example/token","scopes_supported":["repo","read:user"]})";
  std::string post_url;
  std::string post_body;
  std::vector<std::string> post_headers;
  std::vector<std::string> get_urls;
  std::vector<std::string> get_headers;
};

TEST(OAuthDiscoveryTest, DiscoversEndpointsFromWwwAuthenticateChallenge) {
  FakeHttpClient http;

  auto discovered = DiscoverOAuthEndpoints(&http, "https://api.example/mcp");

  ASSERT_TRUE(discovered.ok()) << discovered.status();
  EXPECT_EQ(http.post_url, "https://api.example/mcp");
  ASSERT_EQ(http.get_urls.size(), 2);
  EXPECT_EQ(http.get_urls[0], "https://api.example/.well-known/oauth-protected-resource");
  EXPECT_EQ(http.get_urls[1], "https://auth.example/.well-known/oauth-authorization-server");
  EXPECT_EQ(discovered->resource_metadata_url, "https://api.example/.well-known/oauth-protected-resource");
  EXPECT_EQ(discovered->authorization_server_url, "https://auth.example");
  EXPECT_EQ(discovered->authorization_endpoint, "https://auth.example/authorize");
  EXPECT_EQ(discovered->token_endpoint, "https://auth.example/token");
  ASSERT_EQ(discovered->scopes_supported.size(), 2);
  EXPECT_EQ(discovered->scopes_supported[0], "repo");
}

TEST(OAuthDiscoveryTest, InsertsWellKnownBeforePathIssuer) {
  FakeHttpClient http;
  http.resource_metadata_body =
      R"({"resource":"https://api.example/mcp","authorization_servers":["https://auth.example/tenant-a"]})";
  http.authorization_metadata_url = "https://auth.example/.well-known/oauth-authorization-server/tenant-a";
  http.authorization_metadata_body =
      R"({"issuer":"https://auth.example/tenant-a","authorization_endpoint":"https://auth.example/tenant-a/authorize","token_endpoint":"https://auth.example/tenant-a/token"})";

  auto discovered = DiscoverOAuthEndpoints(&http, "https://api.example/mcp");

  ASSERT_TRUE(discovered.ok()) << discovered.status();
  ASSERT_EQ(http.get_urls.size(), 2);
  EXPECT_EQ(http.get_urls[1], "https://auth.example/.well-known/oauth-authorization-server/tenant-a");
  EXPECT_EQ(discovered->authorization_endpoint, "https://auth.example/tenant-a/authorize");
  EXPECT_EQ(discovered->token_endpoint, "https://auth.example/tenant-a/token");
}

TEST(OAuthDiscoveryTest, RejectsMissingWwwAuthenticateHeader) {
  FakeHttpClient http;
  http.post_response.headers.clear();

  auto discovered = DiscoverOAuthEndpoints(&http, "https://api.example/mcp");

  ASSERT_FALSE(discovered.ok());
  EXPECT_TRUE(absl::IsUnauthenticated(discovered.status()));
}

TEST(OAuthDiscoveryTest, RejectsNonChallengeResponse) {
  FakeHttpClient http;
  http.post_response.status_code = 200;

  auto discovered = DiscoverOAuthEndpoints(&http, "https://api.example/mcp");

  ASSERT_FALSE(discovered.ok());
  EXPECT_TRUE(absl::IsUnauthenticated(discovered.status()));
}

TEST(OAuthDiscoveryTest, RejectsMalformedResourceMetadataJson) {
  FakeHttpClient http;
  http.resource_metadata_body = "{";

  auto discovered = DiscoverOAuthEndpoints(&http, "https://api.example/mcp");

  ASSERT_FALSE(discovered.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(discovered.status()));
}

TEST(OAuthDiscoveryTest, RejectsMultipleAuthorizationServers) {
  FakeHttpClient http;
  http.resource_metadata_body =
      R"({"resource":"https://api.example/mcp","authorization_servers":["https://auth.example","https://login.example"]})";

  auto discovered = DiscoverOAuthEndpoints(&http, "https://api.example/mcp");

  ASSERT_FALSE(discovered.ok());
  EXPECT_TRUE(absl::IsFailedPrecondition(discovered.status()));
}

TEST(OAuthDiscoveryTest, RejectsHttpAuthorizationServer) {
  FakeHttpClient http;
  http.resource_metadata_body = R"({"resource":"https://api.example/mcp","authorization_servers":["http://auth.example"]})";

  auto discovered = DiscoverOAuthEndpoints(&http, "https://api.example/mcp");

  ASSERT_FALSE(discovered.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(discovered.status()));
}

TEST(OAuthDiscoveryTest, RejectsMalformedAuthorizationMetadataJson) {
  FakeHttpClient http;
  http.authorization_metadata_body = "not json";

  auto discovered = DiscoverOAuthEndpoints(&http, "https://api.example/mcp");

  ASSERT_FALSE(discovered.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(discovered.status()));
}

}  // namespace
}  // namespace slop::mcp
