#include "mcp/token_store.h"

#include <filesystem>

#include "gtest/gtest.h"

namespace slop::mcp {
namespace {

std::string TestTokenPath() {
  return (std::filesystem::temp_directory_path() / "slop_mcp_token_store_test.json").string();
}

TEST(TokenStoreTest, SavesLoadsAndDeletesTokens) {
  const std::string path = TestTokenPath();
  std::filesystem::remove(path);
  const OAuthTokenSet input{"access", "refresh", 123};
  ASSERT_TRUE(SaveOAuthTokens(path, input).ok());
  auto loaded = LoadOAuthTokens(path);
  ASSERT_TRUE(loaded.ok());
  EXPECT_EQ(loaded->access_token, "access");
  EXPECT_EQ(loaded->refresh_token, "refresh");
  EXPECT_EQ(loaded->expires_at_unix_seconds, 123);
  ASSERT_TRUE(DeleteOAuthTokens(path).ok());
  EXPECT_FALSE(LoadOAuthTokens(path).ok());
}

TEST(TokenStoreTest, RejectsMissingAccessToken) {
  EXPECT_FALSE(SaveOAuthTokens(TestTokenPath(), {"", "refresh", 0}).ok());
}

TEST(TokenStoreTest, RejectsAccessTokenHeaderControlCharacters) {
  EXPECT_FALSE(SaveOAuthTokens(TestTokenPath(), {"access\nAuthorization: Bearer injected", "refresh", 0}).ok());
}

}  // namespace
}  // namespace slop::mcp
