#include "oauth_handler.h"
#include "http_client.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <cstdlib>
#include <fstream>
#include "absl/strings/match.h"

namespace slop {

using ::testing::_;
using ::testing::Return;
using ::testing::Contains;
using ::testing::AllOf;
using ::testing::HasSubstr;

class MockHttpClient : public HttpClient {
 public:
  MOCK_METHOD(absl::StatusOr<std::string>, Post, (const std::string&, const std::string&, const std::vector<std::string>&), (override));
  MOCK_METHOD(absl::StatusOr<std::string>, Get, (const std::string&, const std::vector<std::string>&), (override));
};

class OAuthHandlerTest : public ::testing::Test {
 protected:
  MockHttpClient mock_http;
};

TEST_F(OAuthHandlerTest, TokenPathSelection) {
  setenv("HOME", "/tmp", 1);
  {
    OAuthHandler handler(&mock_http, OAuthMode::GEMINI);
    EXPECT_TRUE(absl::EndsWith(handler.GetTokenPath(), "/.config/slop/token.json"));
  }
  {
    OAuthHandler handler(&mock_http, OAuthMode::ANTIGRAVITY);
    EXPECT_TRUE(absl::EndsWith(handler.GetTokenPath(), "/.config/slop/antigravity_token.json"));
  }
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
      AllOf(Contains(HasSubstr("X-Goog-Api-Client")),
            Contains(HasSubstr("Client-Metadata")))))
      .WillOnce(Return(object_json));

  auto proj_or = handler.GetProjectId();
  ASSERT_TRUE(proj_or.ok());
  EXPECT_EQ(*proj_or, "managed-project-456");

  unlink(temp_path);
}

TEST_F(OAuthHandlerTest, AntigravityFallbackProject) {
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

  TestOAuthHandler handler(&mock_http, OAuthMode::ANTIGRAVITY);
  handler.SetTokenPath(temp_path);
  handler.SetEnabled(true);

  // Mock discovery failure (404 or empty response)
  EXPECT_CALL(mock_http, Post(HasSubstr("loadCodeAssist"), _, _))
      .WillOnce(Return(absl::InternalError("Not found")));

  // Should fallback to rising-fact-p41fc
  auto proj_or = handler.GetProjectId();
  ASSERT_TRUE(proj_or.ok());
  EXPECT_EQ(*proj_or, "rising-fact-p41fc");

  unlink(temp_path);
}

TEST_F(OAuthHandlerTest, AntigravityDiscoveryEndpoint) {
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

  TestOAuthHandler handler(&mock_http, OAuthMode::ANTIGRAVITY);
  handler.SetTokenPath(temp_path);
  handler.SetEnabled(true);

  // Verify it hits the sandbox endpoint
  EXPECT_CALL(mock_http, Post(HasSubstr("daily-cloudcode-pa.sandbox.googleapis.com"), _, _))
      .WillOnce(Return(R"({"cloudaicompanionProject": "sandbox-proj"})"));

  auto proj_or = handler.GetProjectId();
  ASSERT_TRUE(proj_or.ok());
  EXPECT_EQ(*proj_or, "sandbox-proj");

  unlink(temp_path);
}

} // namespace slop
