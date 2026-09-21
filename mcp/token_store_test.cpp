#include "mcp/token_store.h"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <string>

#include "mcp/token_store_internal.h"

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

TEST(TokenStoreTest, RejectsSymlinkWithoutFollowingIt) {
  const std::string target = TestTokenPath() + ".target";
  const std::string link = TestTokenPath() + ".link";
  std::filesystem::remove(target);
  std::filesystem::remove(link);
  OAuthTokenSet tokens;
  tokens.access_token = "access";
  ASSERT_TRUE(SaveOAuthTokens(target, tokens).ok());
  std::error_code error;
  std::filesystem::create_symlink(target, link, error);
  ASSERT_FALSE(error) << error.message();
  EXPECT_EQ(LoadOAuthTokens(link).status().code(),
            absl::StatusCode::kPermissionDenied);
  std::filesystem::remove(link);
  std::filesystem::remove(target);
}

TEST(TokenStoreTest, WriteAllRetriesInterruptsAndPartialWrites) {
  std::string written;
  int call = 0;
  const absl::Status status = token_store_internal::WriteAll(
      7, "abcdef", [&](int fd, const void* data, size_t size) -> ssize_t {
        EXPECT_EQ(fd, 7);
        ++call;
        if (call == 1) {
          errno = EINTR;
          return -1;
        }
        const size_t count = std::min(size, size_t{2});
        written.append(static_cast<const char*>(data), count);
        return static_cast<ssize_t>(count);
      });
  EXPECT_TRUE(status.ok()) << status;
  EXPECT_EQ(written, "abcdef");
  EXPECT_EQ(call, 4);
}

TEST(TokenStoreTest, WriteAllRejectsZeroProgress) {
  const absl::Status status = token_store_internal::WriteAll(
      7, "data", [](int, const void*, size_t) -> ssize_t { return 0; });
  EXPECT_EQ(status.code(), absl::StatusCode::kUnavailable);
}

TEST(TokenStoreTest, RejectsAccessTokenHeaderControlCharacters) {
  OAuthTokenSet tokens;
  tokens.access_token = "access\nAuthorization: Bearer injected";
  tokens.refresh_token = "refresh";
  EXPECT_FALSE(SaveOAuthTokens(TestTokenPath(), tokens).ok());
}

}  // namespace
}  // namespace slop::mcp
