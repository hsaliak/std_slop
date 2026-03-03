#include "core/oauth_handler.h"

#include <cstdlib>
#include <fstream>

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

TEST_F(OAuthHandlerTest, TokenPathSelection) {
  setenv("HOME", "/tmp", 1);
  {
    OAuthHandler handler(&mock_http);
    EXPECT_TRUE(absl::EndsWith(handler.GetTokenPath(), "/.config/slop/token.json"));
  }
}

TEST_F(OAuthHandlerTest, OpenAiTokenPathSelection) {
  setenv("HOME", "/tmp", 1);
  OAuthHandler handler(&mock_http, OAuthHandler::Provider::kOpenAi);
  EXPECT_TRUE(absl::EndsWith(handler.GetTokenPath(), "/.config/slop/chatgpt_plus_token.json"));
}

TEST_F(OAuthHandlerTest, DiscoverProjectIdObjectFormat) {
  char temp_path[] = "/tmp/slop_test_token_XXXXXX";
  int fd = mkstemp(temp_path);
  close(fd);

  {
    std::ofstream f(temp_path);
    f << R"({"access_token": "fake_token", "refresh_token": "fake_refresh", "expiry_time": 9999999999})";
  }

  class TestOAuthHandler : public OAuthHandler {
   public:
    using OAuthHandler::OAuthHandler;
    void SetTokenPath(const std::string& path) { token_path_ = path; }
  };

  TestOAuthHandler handler(&mock_http);
  handler.SetTokenPath(temp_path);
  handler.SetEnabled(true);

  // 1. Test Object Format Discovery
  std::string object_json = R"({"cloudaicompanionProject": {"id": "managed-project-456"}})";

  // Expect headers verification as well
  EXPECT_CALL(mock_http, Post(HasSubstr("loadCodeAssist"), _,
                              AllOf(Contains(HasSubstr("X-Goog-Api-Client")), Contains(HasSubstr("Client-Metadata")))))
      .WillOnce(Return(object_json));

  auto proj_or = handler.GetProjectId();
  ASSERT_TRUE(proj_or.ok());
  EXPECT_EQ(*proj_or, "managed-project-456");

  unlink(temp_path);
}

TEST_F(OAuthHandlerTest, OpenAiAccountIdFromJwtClaims) {
  char temp_path[] = "/tmp/slop_openai_token_XXXXXX";
  int fd = mkstemp(temp_path);
  close(fd);

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
  close(fd);

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

}  // namespace slop
