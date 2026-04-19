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

  void SleepForDevicePoll(std::chrono::seconds delay) const override {
    sleep_calls.push_back(delay.count());
  }

  mutable std::vector<int64_t> sleep_calls;
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

TEST_F(OAuthHandlerTest, FetchOpenAiDeviceTokenRejectsMissingDeviceAuthId) {
  OAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);
  OAuthHandler::DeviceAuthorizationStart start;
  std::ostringstream out;
  auto status = handler.FetchOpenAiDeviceToken(start, out);
  ASSERT_FALSE(status.ok());
  EXPECT_TRUE(absl::StrContains(status.message(), "Device authorization ID is required"));
}

TEST_F(OAuthHandlerTest, FetchOpenAiDeviceTokenRejectsMissingRefreshToken) {
  SleepCapturingOAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);
  handler.SetTokenPath("/tmp/slop_openai_device_token_missing_refresh.json");
  OAuthHandler::DeviceAuthorizationStart start;
  start.device_auth_id = "auth-123";
  start.interval_seconds = 1;
  start.expires_in_seconds = 10;
  std::ostringstream out;

  EXPECT_CALL(mock_http, Post(HasSubstr("deviceauth/token"), _, Contains("Content-Type: application/json")))
      .WillOnce(Return(R"({"access_token":"token-only","expires_in":3600})"));

  auto status = handler.FetchOpenAiDeviceToken(start, out);
  ASSERT_FALSE(status.ok());
  EXPECT_TRUE(absl::StrContains(status.message(), "missing required refresh_token"));
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
  start.interval_seconds = 1;
  start.expires_in_seconds = 10;
  std::ostringstream out;

  EXPECT_CALL(mock_http, Post(HasSubstr("deviceauth/token"), _, Contains("Content-Type: application/json")))
      .WillOnce(Return(absl::UnauthenticatedError("Terminal HTTP error: 400 {\"error\":\"authorization_pending\"}")))
      .WillOnce(Return(absl::UnauthenticatedError("Terminal HTTP error: 400 {\"error\":\"slow_down\"}")))
      .WillOnce(Return(R"({"access_token":"header.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoib3JnX3Rlc3QifX0.sig","refresh_token":"refresh-123","expires_in":3600})"));

  auto status = handler.FetchOpenAiDeviceToken(start, out);
  ASSERT_TRUE(status.ok());
  ASSERT_EQ(handler.sleep_calls.size(), 2);
  EXPECT_EQ(handler.sleep_calls[0], 1);
  EXPECT_EQ(handler.sleep_calls[1], 2);

  unlink(temp_path);
}

TEST_F(OAuthHandlerTest, MissingTokenFileGuidanceMentionsFetchOauth) {
  OAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);
  handler.SetEnabled(true);
  handler.SetTokenPath("/tmp/slop_missing_openai_oauth_token.json");

  auto token_or = handler.GetValidToken();
  ASSERT_FALSE(token_or.ok());
  EXPECT_TRUE(absl::StrContains(token_or.status().message(), "Run: std_slop --fetch-oauth"));
}

}  // namespace slop
