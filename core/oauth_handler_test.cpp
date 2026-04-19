#include "core/oauth_handler.h"

#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#include "absl/strings/match.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "core/http_client.h"

namespace slop {

using ::testing::_;
using ::testing::AllOf;
using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::Return;

class MockHttpClient : public HttpClient {
 public:
  MOCK_METHOD(absl::StatusOr<std::string>, Post,
              (const std::string&, const std::string&, const std::vector<std::string>&), (override));
  MOCK_METHOD(absl::StatusOr<std::string>, Get, (const std::string&, const std::vector<std::string>&), (override));
};

class OAuthHandlerTest : public ::testing::Test {
 protected:
  MockHttpClient mock_http;
};

class SleepCapturingOAuthHandler : public OAuthHandler {
 public:
  using OAuthHandler::OAuthHandler;

  void SetRandomValues(std::vector<std::string> values) { random_values = std::move(values); }

  void SetPkceCodeChallenge(std::string value) { pkce_code_challenge = std::move(value); }

  absl::StatusOr<std::string> ComputePkceCodeChallengeForTest(const std::string& code_verifier) const {
    return OAuthHandler::BuildPkceCodeChallenge(code_verifier);
  }

  std::string GenerateOAuthRandomValue() const override {
    if (random_values.empty()) {
      return "default-random-value";
    }
    const std::string value = random_values.front();
    random_values.erase(random_values.begin());
    return value;
  }

  absl::StatusOr<std::string> BuildPkceCodeChallenge(const std::string& code_verifier) const override {
    seen_code_verifier = code_verifier;
    return pkce_code_challenge;
  }

  void SleepForDevicePoll(std::chrono::seconds delay) const override {
    sleep_calls.push_back(delay.count());
  }

  mutable std::vector<int64_t> sleep_calls;
  mutable std::vector<std::string> random_values;
  mutable std::string seen_code_verifier;
  std::string pkce_code_challenge = "challenge-123";
};

TEST_F(OAuthHandlerTest, OpenAiTokenPathSelection) {
  setenv("HOME", "/tmp", 1);
  OAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);
  EXPECT_TRUE(absl::EndsWith(handler.GetTokenPath(), "/.config/slop/chatgpt_plus_token.json"));
}

TEST_F(OAuthHandlerTest, OpenAiAccountIdFromJwtClaims) {
  char temp_path[] = "/tmp/slop_openai_token_XXXXXX";
  int fd = mkstemp(temp_path);
  if (fd != -1) close(fd);

  {
    std::ofstream f(temp_path);
    f << R"({
  "access_token": "eyJhbGciOiJub25lIiwidHlwIjoiSldUIn0.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoib3JnX3Rlc3RfMTIzIn19.c2ln",
  "refresh_token": "fake_refresh",
  "expiry_time": 9999999999
})";
  }

  class TestOAuthHandler : public OAuthHandler {
   public:
    using OAuthHandler::OAuthHandler;
    void SetTokenPath(const std::string& path) { token_path_ = path; }
  };

  TestOAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);
  handler.SetTokenPath(temp_path);
  handler.SetEnabled(true);

  auto account_or = handler.GetOpenAiAccountId();
  ASSERT_TRUE(account_or.ok());
  EXPECT_EQ(*account_or, "org_test_123");

  unlink(temp_path);
}

TEST_F(OAuthHandlerTest, OpenAiRefreshRequestFormValuesAreUrlEncoded) {
  char temp_path[] = "/tmp/slop_openai_refresh_token_XXXXXX";
  int fd = mkstemp(temp_path);
  if (fd != -1) close(fd);

  {
    std::ofstream f(temp_path);
    f << R"({
  "access_token": "stale_token",
  "refresh_token": "refresh+tok&en=1 /",
  "expiry_time": 1
})";
  }

  class TestOAuthHandler : public OAuthHandler {
   public:
    using OAuthHandler::OAuthHandler;
    void SetTokenPath(const std::string& path) { token_path_ = path; }
  };

  setenv("CHATGPT_CLIENT_SECRET", "sec+ret&x=y /", 1);

  TestOAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);
  handler.SetTokenPath(temp_path);
  handler.SetEnabled(true);

  EXPECT_CALL(mock_http, Post(HasSubstr("oauth/token"),
                              AllOf(HasSubstr("refresh_token=refresh%2Btok%26en%3D1%20%2F"),
                                    HasSubstr("client_secret=sec%2Bret%26x%3Dy%20%2F"), HasSubstr("client_id="),
                                    HasSubstr("grant_type=refresh_token")),
                              Contains("Content-Type: application/x-www-form-urlencoded")))
      .WillOnce(Return(R"({"access_token":"new_token","expires_in":3600})"));

  auto token_or = handler.GetValidToken();
  ASSERT_TRUE(token_or.ok());
  EXPECT_EQ(*token_or, "new_token");

  unsetenv("CHATGPT_CLIENT_SECRET");
  unlink(temp_path);
}

TEST_F(OAuthHandlerTest, StartOpenAiDeviceAuthorizationParsesRequiredFields) {
  OAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);

  EXPECT_CALL(mock_http, Post(HasSubstr("deviceauth/usercode"), HasSubstr("client_id"),
                              Contains("Content-Type: application/json")))
      .WillOnce(Return(R"({"device_auth_id":"auth-123","user_code":"ABCD-EFGH","verification_uri":"https://auth.openai.com/codex/device","interval":7,"expires_in":1234})"));

  auto start_or = handler.StartOpenAiDeviceAuthorization();
  ASSERT_TRUE(start_or.ok());
  EXPECT_EQ(start_or->device_auth_id, "auth-123");
  EXPECT_EQ(start_or->user_code, "ABCD-EFGH");
  EXPECT_EQ(start_or->verification_uri, "https://auth.openai.com/codex/device");
  EXPECT_EQ(start_or->interval_seconds, 7);
  EXPECT_EQ(start_or->expires_in_seconds, 1234);
}

TEST_F(OAuthHandlerTest, StartOpenAiManualAuthorizationBuildsBrowserUrl) {
  SleepCapturingOAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);
  handler.SetRandomValues({"state-value", "verifier-value"});
  handler.SetPkceCodeChallenge("pkce-challenge");

  auto session_or = handler.StartOpenAiManualAuthorization();
  ASSERT_TRUE(session_or.ok());
  EXPECT_EQ(session_or->state, "state-value");
  EXPECT_EQ(session_or->code_verifier, "verifier-value");
  EXPECT_EQ(handler.seen_code_verifier, "verifier-value");
  EXPECT_EQ(session_or->redirect_uri, "http://localhost:1455/auth/callback");
  EXPECT_TRUE(absl::StrContains(session_or->authorization_uri, "https://auth.openai.com/oauth/authorize?"));
  EXPECT_TRUE(absl::StrContains(session_or->authorization_uri, "code_challenge=pkce-challenge"));
  EXPECT_TRUE(absl::StrContains(session_or->authorization_uri, "state=state-value"));
  EXPECT_TRUE(absl::StrContains(session_or->authorization_uri, "redirect_uri=http%3A%2F%2Flocalhost%3A1455%2Fauth%2Fcallback"));
}

TEST_F(OAuthHandlerTest, CompleteOpenAiManualAuthorizationExchangesCallbackCode) {
  SleepCapturingOAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);
  char temp_path[] = "/tmp/slop_openai_manual_token_XXXXXX";
  int fd = mkstemp(temp_path);
  if (fd != -1) close(fd);
  unlink(temp_path);
  handler.SetTokenPath(temp_path);

  OAuthHandler::ManualAuthorizationSession session;
  session.state = "state-123";
  session.code_verifier = "verifier 123";
  session.redirect_uri = "http://localhost:1455/auth/callback";

  EXPECT_CALL(mock_http, Post(HasSubstr("oauth/token"),
                              AllOf(HasSubstr("grant_type=authorization_code"), HasSubstr("code=auth-code"),
                                    HasSubstr("code_verifier=verifier%20123"),
                                    HasSubstr("redirect_uri=http%3A%2F%2Flocalhost%3A1455%2Fauth%2Fcallback")),
                              Contains("Content-Type: application/x-www-form-urlencoded")))
      .WillOnce(Return(R"({"access_token":"header.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoib3JnX21hbnVhbCJ9fQ.sig","refresh_token":"refresh-123","expires_in":3600})"));

  std::ostringstream out;
  auto status = handler.CompleteOpenAiManualAuthorization(
      session, "http://localhost:1455/auth/callback?code=auth-code&state=state-123", out);
  ASSERT_TRUE(status.ok());
  EXPECT_TRUE(absl::StrContains(out.str(), "OpenAI OAuth token saved to"));

  unlink(temp_path);
}

TEST_F(OAuthHandlerTest, CompleteOpenAiManualAuthorizationRejectsStateMismatch) {
  SleepCapturingOAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);
  OAuthHandler::ManualAuthorizationSession session;
  session.state = "expected-state";
  session.code_verifier = "verifier-123";
  session.redirect_uri = "http://localhost:1455/auth/callback";
  std::ostringstream out;

  auto status = handler.CompleteOpenAiManualAuthorization(
      session, "http://localhost:1455/auth/callback?code=auth-code&state=wrong-state", out);
  ASSERT_FALSE(status.ok());
  EXPECT_TRUE(absl::StrContains(status.message(), "state mismatch"));
}

TEST_F(OAuthHandlerTest, CompleteOpenAiManualAuthorizationRejectsMissingState) {
  SleepCapturingOAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);
  OAuthHandler::ManualAuthorizationSession session;
  session.state = "expected-state";
  session.code_verifier = "verifier-123";
  session.redirect_uri = "http://localhost:1455/auth/callback";
  std::ostringstream out;

  auto status = handler.CompleteOpenAiManualAuthorization(
      session, "http://localhost:1455/auth/callback?code=auth-code", out);
  ASSERT_FALSE(status.ok());
  EXPECT_TRUE(absl::StrContains(status.message(), "state parameter"));
}

TEST_F(OAuthHandlerTest, BuildPkceCodeChallengeMatchesKnownValue) {
  SleepCapturingOAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);

  auto challenge_or = handler.ComputePkceCodeChallengeForTest("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
  ASSERT_TRUE(challenge_or.ok());
  EXPECT_EQ(*challenge_or, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
}

TEST_F(OAuthHandlerTest, FetchOpenAiDeviceTokenRejectsMissingDeviceAuthId) {
  OAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);
  OAuthHandler::DeviceAuthorizationStart start;
  std::ostringstream out;
  auto status = handler.FetchOpenAiDeviceToken(start, out);
  ASSERT_FALSE(status.ok());
  EXPECT_TRUE(absl::StrContains(status.message(), "Device authorization ID is required"));
}

TEST_F(OAuthHandlerTest, FetchOpenAiDeviceTokenRejectsMissingUserCode) {
  OAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);
  OAuthHandler::DeviceAuthorizationStart start;
  start.device_auth_id = "auth-123";
  std::ostringstream out;

  auto status = handler.FetchOpenAiDeviceToken(start, out);
  ASSERT_FALSE(status.ok());
  EXPECT_TRUE(absl::StrContains(status.message(), "user code is required"));
}

TEST_F(OAuthHandlerTest, FetchOpenAiDeviceTokenRejectsMissingCodeVerifier) {
  SleepCapturingOAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);
  handler.SetTokenPath("/tmp/slop_openai_device_token_missing_refresh.json");
  OAuthHandler::DeviceAuthorizationStart start;
  start.device_auth_id = "auth-123";
  start.user_code = "USER-CODE";
  start.interval_seconds = 1;
  start.expires_in_seconds = 10;
  std::ostringstream out;

  EXPECT_CALL(mock_http, Post(HasSubstr("deviceauth/token"), HasSubstr("user_code"),
                              Contains("Content-Type: application/json")))
      .WillOnce(Return(R"({"authorization_code":"device-code-only"})"));

  auto status = handler.FetchOpenAiDeviceToken(start, out);
  ASSERT_FALSE(status.ok());
  EXPECT_TRUE(absl::StrContains(status.message(), "missing authorization_code/code_verifier"));
}

TEST_F(OAuthHandlerTest, FetchOpenAiDeviceTokenBacksOffAfterSlowDown) {
  SleepCapturingOAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);
  char temp_path[] = "/tmp/slop_openai_device_token_XXXXXX";
  int fd = mkstemp(temp_path);
  if (fd != -1) close(fd);
  unlink(temp_path);
  handler.SetTokenPath(temp_path);

  OAuthHandler::DeviceAuthorizationStart start;
  start.device_auth_id = "auth-123";
  start.user_code = "USER-CODE";
  start.interval_seconds = 1;
  start.expires_in_seconds = 10;
  std::ostringstream out;

  EXPECT_CALL(mock_http, Post(HasSubstr("deviceauth/token"), HasSubstr("user_code"), Contains("Content-Type: application/json")))
      .WillOnce(Return(absl::UnauthenticatedError("Terminal HTTP error: 400 {\"error\":\"authorization_pending\"}")))
      .WillOnce(Return(absl::UnauthenticatedError("Terminal HTTP error: 400 {\"error\":\"slow_down\"}")))
      .WillOnce(Return(R"({"authorization_code":"device-code-123","code_verifier":"device-verifier-123"})"));
  EXPECT_CALL(mock_http, Post(HasSubstr("oauth/token"),
                              AllOf(HasSubstr("code=device-code-123"), HasSubstr("code_verifier=device-verifier-123"),
                                    HasSubstr("redirect_uri=https%3A%2F%2Fauth.openai.com%2Fdeviceauth%2Fcallback")),
                              Contains("Content-Type: application/x-www-form-urlencoded")))
      .WillOnce(Return(R"({"access_token":"header.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoib3JnX3Rlc3QifX0.sig","refresh_token":"refresh-123","expires_in":3600})"));

  auto status = handler.FetchOpenAiDeviceToken(start, out);
  ASSERT_TRUE(status.ok());
  ASSERT_EQ(handler.sleep_calls.size(), 2);
  EXPECT_EQ(handler.sleep_calls[0], 4);
  EXPECT_EQ(handler.sleep_calls[1], 5);

  unlink(temp_path);
}

TEST_F(OAuthHandlerTest, FetchOpenAiDeviceTokenRetriesAfter403Unknown) {
  SleepCapturingOAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);
  char temp_path[] = "/tmp/slop_openai_device_token_403_XXXXXX";
  int fd = mkstemp(temp_path);
  if (fd != -1) close(fd);
  unlink(temp_path);
  handler.SetTokenPath(temp_path);

  OAuthHandler::DeviceAuthorizationStart start;
  start.device_auth_id = "auth-403";
  start.user_code = "USER-CODE";
  start.interval_seconds = 2;
  start.expires_in_seconds = 10;
  std::ostringstream out;

  EXPECT_CALL(mock_http, Post(HasSubstr("deviceauth/token"), HasSubstr("user_code"), Contains("Content-Type: application/json")))
      .WillOnce(Return(absl::UnauthenticatedError(
          "Terminal HTTP error: 403 Body: {\"error\":{\"code\":\"deviceauth_authorization_unknown\"}}")))
      .WillOnce(Return(R"({"authorization_code":"device-code-403","code_verifier":"device-verifier-403"})"));
  EXPECT_CALL(mock_http, Post(HasSubstr("oauth/token"),
                              AllOf(HasSubstr("code=device-code-403"), HasSubstr("code_verifier=device-verifier-403"),
                                    HasSubstr("redirect_uri=https%3A%2F%2Fauth.openai.com%2Fdeviceauth%2Fcallback")),
                              Contains("Content-Type: application/x-www-form-urlencoded")))
      .WillOnce(Return(R"({"access_token":"header.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoib3JnXzQwMyJ9fQ.sig","refresh_token":"refresh-403","expires_in":3600})"));

  auto status = handler.FetchOpenAiDeviceToken(start, out);
  ASSERT_TRUE(status.ok());
  ASSERT_EQ(handler.sleep_calls.size(), 1);
  EXPECT_EQ(handler.sleep_calls[0], 5);

  unlink(temp_path);
}

TEST_F(OAuthHandlerTest, FetchOpenAiDeviceTokenFailsFastOnNonRetryableError) {
  SleepCapturingOAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);
  OAuthHandler::DeviceAuthorizationStart start;
  start.device_auth_id = "auth-fail";
  start.user_code = "USER-CODE";
  start.interval_seconds = 1;
  start.expires_in_seconds = 10;
  std::ostringstream out;

  EXPECT_CALL(mock_http, Post(HasSubstr("deviceauth/token"), HasSubstr("user_code"), Contains("Content-Type: application/json")))
      .WillOnce(Return(absl::UnauthenticatedError("Terminal HTTP error: 401 Body: {}")));

  auto status = handler.FetchOpenAiDeviceToken(start, out);
  ASSERT_FALSE(status.ok());
  EXPECT_TRUE(absl::StrContains(status.message(), "OpenAI device authorization failed"));
  EXPECT_TRUE(handler.sleep_calls.empty());
}

TEST_F(OAuthHandlerTest, MissingTokenFileGuidanceMentionsFetchOpenAiOauthToken) {
  OAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);
  handler.SetEnabled(true);
  handler.SetTokenPath("/tmp/slop_missing_openai_oauth_token.json");

  auto token_or = handler.GetValidToken();
  ASSERT_FALSE(token_or.ok());
  EXPECT_TRUE(absl::StrContains(token_or.status().message(), "Run: std_slop --fetch_openai_oauth_token"));
}

}  // namespace slop
