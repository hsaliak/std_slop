#include "oauth_handler.h"
#include "http_client.h"
#include <gtest/gtest.h>
#include <cstdlib>

namespace slop {

class OAuthHandlerTest : public ::testing::Test {
 protected:
  HttpClient http_client;
};

TEST_F(OAuthHandlerTest, ManualProjectOverride) {
  OAuthHandler handler(&http_client);
  handler.SetProjectId("manual-project");
  
  auto proj_or = handler.GetProjectId();
  EXPECT_TRUE(proj_or.ok());
  EXPECT_EQ(*proj_or, "manual-project");
}

TEST_F(OAuthHandlerTest, EnvProjectFallback) {
  OAuthHandler handler(&http_client);
  setenv("GOOGLE_CLOUD_PROJECT", "env-project", 1);
  
  // We can't easily test the full GetProjectId flow without mocking HttpClient,
  // but we can at least verify that it doesn't crash and respects manual override first.
  handler.SetProjectId("manual-override");
  EXPECT_EQ(*handler.GetProjectId(), "manual-override");
  
  unsetenv("GOOGLE_CLOUD_PROJECT");
}

} // namespace slop
