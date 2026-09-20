#include "mcp/token_store.h"

#include <filesystem>

#include "gtest/gtest.h"

#include <sys/stat.h>

namespace slop::mcp {
namespace {

std::string TestTokenPath() {
  return (std::filesystem::temp_directory_path() / "slop_mcp_token_store_test.json").string();
}

TEST(TokenStoreTest, SavesLoadsAndDeletesTokens) {
  const std::string path = TestTokenPath();
  std::filesystem::remove(path);
  OAuthTokenSet input;
  input.access_token = "access";
  input.refresh_token = "refresh";
  input.issuer = "https://auth.example";
  input.resource = "https://api.example/mcp";
  input.expires_at_unix_seconds = 123;
  ASSERT_TRUE(SaveOAuthTokens(path, input).ok());
  auto loaded = LoadOAuthTokens(path);
  ASSERT_TRUE(loaded.ok());
  EXPECT_EQ(loaded->access_token, "access");
  EXPECT_EQ(loaded->refresh_token, "refresh");
  EXPECT_EQ(loaded->token_type, "Bearer");
  EXPECT_EQ(loaded->issuer, "https://auth.example");
  EXPECT_EQ(loaded->resource, "https://api.example/mcp");
  EXPECT_EQ(loaded->expires_at_unix_seconds, 123);
  ASSERT_TRUE(DeleteOAuthTokens(path).ok());
  EXPECT_FALSE(LoadOAuthTokens(path).ok());
}

TEST(TokenStoreTest, RejectsMissingAccessToken) {
  OAuthTokenSet tokens;
  tokens.refresh_token = "refresh";
  EXPECT_FALSE(SaveOAuthTokens(TestTokenPath(), tokens).ok());
}

TEST(TokenStoreTest, RejectsInsecureTokenFilePermissions) {
  const std::string path = TestTokenPath();
  std::filesystem::remove(path);
  OAuthTokenSet tokens;
  tokens.access_token = "access";
  ASSERT_TRUE(SaveOAuthTokens(path, tokens).ok());
  ASSERT_EQ(chmod(path.c_str(), 0644), 0);
  EXPECT_EQ(LoadOAuthTokens(path).status().code(), absl::StatusCode::kPermissionDenied);
  std::filesystem::remove(path);
}

TEST(TokenStoreTest, RejectsAccessTokenHeaderControlCharacters) {
  OAuthTokenSet tokens;
  tokens.access_token = "access\nAuthorization: Bearer injected";
  tokens.refresh_token = "refresh";
  EXPECT_FALSE(SaveOAuthTokens(TestTokenPath(), tokens).ok());
}

}  // namespace
}  // namespace slop::mcp
