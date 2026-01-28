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

}  // namespace slop
