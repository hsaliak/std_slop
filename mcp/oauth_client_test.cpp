#include "mcp/oauth_client.h"

#include <algorithm>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace slop::mcp {
namespace {

class FakeHttpClient : public HttpClient {
 public:
  absl::StatusOr<HttpResponse> PostWithResponse(const std::string& url, const std::string& body,
                                                const std::vector<std::string>& headers) override {
    last_url = url;
    last_body = body;
    last_headers = headers;
    return response;
  }

  HttpResponse response{200, R"({"access_token":"access","refresh_token":"refresh","expires_in":60})", {}};
  std::string last_url;
  std::string last_body;
  std::vector<std::string> last_headers;
};

OAuthClientConfig Config() {
  OAuthClientConfig config;
  config.client_id = "client";
  config.authorization_endpoint = "https://auth.example/authorize";
  config.token_endpoint = "https://auth.example/token";
  config.scopes = {"repo"};
  return config;
}

TEST(OAuthClientTest, BuildsPkceAuthorizationUrlAndParsesCallback) {
  auto session = StartPkceAuthorization(Config());
  ASSERT_TRUE(session.ok()) << session.status();
  EXPECT_NE(session->authorization_url.find("client_id=client"), std::string::npos);
  EXPECT_NE(session->authorization_url.find("code_challenge_method=S256"), std::string::npos);
  EXPECT_NE(session->authorization_url.find("scope=repo"), std::string::npos);

  auto code = ExtractAuthorizationCodeFromCallback("http://127.0.0.1/callback?code=abc&state=" + session->state,
                                                   session->state);
  ASSERT_TRUE(code.ok());
  EXPECT_EQ(*code, "abc");
}

TEST(OAuthClientTest, RejectsStateMismatch) {
  auto code = ExtractAuthorizationCodeFromCallback("http://127.0.0.1/callback?code=abc&state=bad", "expected");
  EXPECT_FALSE(code.ok());
}

TEST(OAuthClientTest, OmitsEmptyScopeParameter) {
  OAuthClientConfig config = Config();
  config.scopes.clear();
  auto session = StartPkceAuthorization(config);
  ASSERT_TRUE(session.ok()) << session.status();
  EXPECT_EQ(session->authorization_url.find("scope="), std::string::npos);
}

bool HasHeader(const std::vector<std::string>& headers, const std::string& expected) {
  return std::find(headers.begin(), headers.end(), expected) != headers.end();
}

TEST(OAuthClientTest, ExchangesAndRefreshesTokens) {
  FakeHttpClient http;
  auto exchanged = ExchangeAuthorizationCode(&http, Config(), "code", "verifier");
  ASSERT_TRUE(exchanged.ok()) << exchanged.status();
  EXPECT_EQ(exchanged->access_token, "access");
  EXPECT_NE(http.last_body.find("grant_type=authorization_code"), std::string::npos);
  EXPECT_TRUE(HasHeader(http.last_headers, "Accept: application/json"));
  EXPECT_TRUE(HasHeader(http.last_headers, "Content-Type: application/x-www-form-urlencoded"));

  auto refreshed = RefreshOAuthToken(&http, Config(), "refresh");
  ASSERT_TRUE(refreshed.ok()) << refreshed.status();
  EXPECT_NE(http.last_body.find("grant_type=refresh_token"), std::string::npos);
  EXPECT_TRUE(HasHeader(http.last_headers, "Accept: application/json"));
  EXPECT_TRUE(HasHeader(http.last_headers, "Content-Type: application/x-www-form-urlencoded"));
}

}  // namespace
}  // namespace slop::mcp
