#include "mcp/oauth_client.h"

#include <algorithm>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/match.h"
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

TEST(OAuthClientTest, ReportsCallbackOAuthError) {
  auto code = ExtractAuthorizationCodeFromCallback(
      "http://127.0.0.1/callback?error=access_denied&error_description=User+denied&state=expected",
      "expected");
  ASSERT_FALSE(code.ok());
  EXPECT_TRUE(absl::IsUnauthenticated(code.status()));
  EXPECT_TRUE(absl::StrContains(code.status().message(), "access_denied"));
  EXPECT_TRUE(absl::StrContains(code.status().message(), "User denied"));
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

TEST(OAuthClientTest, SendsClientSecretWhenConfigured) {
  FakeHttpClient http;
  OAuthClientConfig config = Config();
  config.client_secret = "secret value";

  auto exchanged = ExchangeAuthorizationCode(&http, config, "code", "verifier");
  ASSERT_TRUE(exchanged.ok()) << exchanged.status();
  EXPECT_NE(http.last_body.find("client_secret=secret%20value"), std::string::npos);

  auto refreshed = RefreshOAuthToken(&http, config, "refresh");
  ASSERT_TRUE(refreshed.ok()) << refreshed.status();
  EXPECT_NE(http.last_body.find("client_secret=secret%20value"), std::string::npos);
}

TEST(OAuthClientTest, OmitsClientSecretWhenNotConfigured) {
  FakeHttpClient http;

  auto exchanged = ExchangeAuthorizationCode(&http, Config(), "code", "verifier");
  ASSERT_TRUE(exchanged.ok()) << exchanged.status();
  EXPECT_EQ(http.last_body.find("client_secret="), std::string::npos);

  auto refreshed = RefreshOAuthToken(&http, Config(), "refresh");
  ASSERT_TRUE(refreshed.ok()) << refreshed.status();
  EXPECT_EQ(http.last_body.find("client_secret="), std::string::npos);
}

TEST(OAuthClientTest, ReportsJsonOAuthErrorResponse) {
  FakeHttpClient http;
  http.response.body =
      R"({"error":"incorrect_client_credentials","error_description":"The client_id and/or client_secret passed are incorrect.","error_uri":"https://docs.example/error"})";

  auto exchanged = ExchangeAuthorizationCode(&http, Config(), "code", "verifier");

  ASSERT_FALSE(exchanged.ok());
  EXPECT_TRUE(absl::IsUnauthenticated(exchanged.status()));
  EXPECT_TRUE(absl::StrContains(exchanged.status().message(), "incorrect_client_credentials"));
  EXPECT_TRUE(absl::StrContains(exchanged.status().message(), "client_secret"));
  EXPECT_TRUE(absl::StrContains(exchanged.status().message(), "https://docs.example/error"));
}

TEST(OAuthClientTest, ReportsJsonOAuthErrorOnNonSuccessStatus) {
  FakeHttpClient http;
  http.response.status_code = 400;
  http.response.body = R"({"error":"bad_verification_code","error_description":"code expired"})";

  auto exchanged = ExchangeAuthorizationCode(&http, Config(), "code", "verifier");

  ASSERT_FALSE(exchanged.ok());
  EXPECT_TRUE(absl::IsUnauthenticated(exchanged.status()));
  EXPECT_TRUE(absl::StrContains(exchanged.status().message(), "bad_verification_code"));
  EXPECT_TRUE(absl::StrContains(exchanged.status().message(), "code expired"));
  EXPECT_TRUE(absl::StrContains(exchanged.status().message(), "HTTP 400"));
}

TEST(OAuthClientTest, ReportsHttpStatusForJsonNonOAuthErrorResponse) {
  FakeHttpClient http;
  http.response.status_code = 500;
  http.response.body = R"({"message":"server failed"})";

  auto exchanged = ExchangeAuthorizationCode(&http, Config(), "code", "verifier");

  ASSERT_FALSE(exchanged.ok());
  EXPECT_TRUE(absl::IsUnauthenticated(exchanged.status()));
  EXPECT_TRUE(absl::StrContains(exchanged.status().message(), "OAuth token exchange failed"));
  EXPECT_TRUE(absl::StrContains(exchanged.status().message(), "HTTP 500"));
}

TEST(OAuthClientTest, MissingAccessTokenMentionsMissingErrorField) {
  FakeHttpClient http;
  http.response.body = R"({"token_type":"bearer"})";

  auto exchanged = ExchangeAuthorizationCode(&http, Config(), "code", "verifier");

  ASSERT_FALSE(exchanged.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(exchanged.status()));
  EXPECT_TRUE(absl::StrContains(exchanged.status().message(), "missing access_token"));
  EXPECT_TRUE(absl::StrContains(exchanged.status().message(), "OAuth error field"));
}

}  // namespace
}  // namespace slop::mcp
