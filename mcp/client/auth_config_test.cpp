#include "mcp/client/auth_config.h"

#include <cstdlib>
#include <filesystem>
#include <string>

#include "gtest/gtest.h"
#include "mcp/client/token_store.h"

namespace slop::mcp {
namespace {

std::string TokenPath(const std::string& name) {
  return (std::filesystem::temp_directory_path() / ("slop_mcp_auth_config_" + name + ".json")).string();
}

HttpAuthConfig BearerFileConfig(const std::string& path) {
  HttpAuthConfig config;
  config.method = HttpAuthMethod::kBearer;
  config.token_file = path;
  return config;
}

OAuthTokenSet OAuthTokens(const std::string& issuer, const std::string& resource) {
  OAuthTokenSet tokens;
  tokens.access_token = "access-token";
  tokens.issuer = issuer;
  tokens.resource = resource;
  return tokens;
}

TEST(HttpAuthConfigTest, NoneResolvesWithoutCredential) {
  auto token = ResolveHttpAuthToken("https://mcp.example/mcp", HttpAuthConfig{});
  ASSERT_TRUE(token.ok()) << token.status();
  EXPECT_FALSE(token->has_value());
}

TEST(HttpAuthConfigTest, BearerTokenFileResolves) {
  const std::string path = TokenPath("bearer_file");
  ASSERT_TRUE(SaveOAuthTokens(path, OAuthTokens("", "")).ok());

  auto token = ResolveHttpAuthToken("https://mcp.example/mcp", BearerFileConfig(path));
  ASSERT_TRUE(token.ok()) << token.status();
  ASSERT_TRUE(token->has_value());
  EXPECT_EQ(**token, "access-token");
  EXPECT_TRUE(DeleteOAuthTokens(path).ok());
}

TEST(HttpAuthConfigTest, BearerEnvironmentTokenResolves) {
  constexpr char kTokenEnv[] = "SLOP_TEST_MCP_BEARER_TOKEN";
  ASSERT_EQ(setenv(kTokenEnv, "environment-token", 1), 0);
  HttpAuthConfig config;
  config.method = HttpAuthMethod::kBearer;
  config.token_env = kTokenEnv;

  auto token = ResolveHttpAuthToken("https://mcp.example/mcp", config);
  ASSERT_TRUE(token.ok()) << token.status();
  ASSERT_TRUE(token->has_value());
  EXPECT_EQ(**token, "environment-token");
  EXPECT_EQ(unsetenv(kTokenEnv), 0);
}

TEST(HttpAuthConfigTest, MissingOrInvalidBearerEnvironmentTokenIsRejected) {
  constexpr char kTokenEnv[] = "SLOP_TEST_MCP_MISSING_BEARER_TOKEN";
  EXPECT_EQ(unsetenv(kTokenEnv), 0);
  HttpAuthConfig config;
  config.method = HttpAuthMethod::kBearer;
  config.token_env = kTokenEnv;
  auto missing_token = ResolveHttpAuthToken("https://mcp.example/mcp", config);
  ASSERT_FALSE(missing_token.ok());
  EXPECT_TRUE(absl::IsUnauthenticated(missing_token.status()));

  ASSERT_EQ(setenv(kTokenEnv, "bad\r\nheader", 1), 0);
  auto invalid_token = ResolveHttpAuthToken("https://mcp.example/mcp", config);
  ASSERT_FALSE(invalid_token.ok());
  EXPECT_TRUE(absl::IsUnauthenticated(invalid_token.status()));
  EXPECT_EQ(unsetenv(kTokenEnv), 0);
}

TEST(HttpAuthConfigTest, RejectsInvalidTokenSourceShapes) {
  HttpAuthConfig bearer;
  bearer.method = HttpAuthMethod::kBearer;
  EXPECT_FALSE(ValidateHttpAuthConfig(bearer).ok());
  bearer.token_file = "token.json";
  bearer.token_env = "TOKEN";
  EXPECT_FALSE(ValidateHttpAuthConfig(bearer).ok());

  HttpAuthConfig none;
  none.token_env = "TOKEN";
  EXPECT_FALSE(ValidateHttpAuthConfig(none).ok());

  HttpAuthConfig oauth;
  oauth.method = HttpAuthMethod::kOAuth;
  oauth.token_file = "token.json";
  EXPECT_FALSE(ValidateHttpAuthConfig(oauth).ok());
}

TEST(HttpAuthConfigTest, OAuthTokenRequiresMatchingIssuerAndResource) {
  const std::string path = TokenPath("oauth_binding");
  HttpAuthConfig config;
  config.method = HttpAuthMethod::kOAuth;
  config.token_file = path;
  config.expected_issuer = "https://auth.example";
  ASSERT_TRUE(SaveOAuthTokens(path, OAuthTokens(config.expected_issuer, "https://mcp.example/mcp")).ok());

  auto token = ResolveHttpAuthToken("https://mcp.example/mcp", config);
  ASSERT_TRUE(token.ok()) << token.status();
  ASSERT_TRUE(token->has_value());
  EXPECT_EQ(**token, "access-token");

  auto wrong_resource = ResolveHttpAuthToken("https://other.example/mcp", config);
  ASSERT_FALSE(wrong_resource.ok());
  EXPECT_TRUE(absl::IsPermissionDenied(wrong_resource.status()));

  config.expected_issuer = "https://other-auth.example";
  auto wrong_issuer = ResolveHttpAuthToken("https://mcp.example/mcp", config);
  ASSERT_FALSE(wrong_issuer.ok());
  EXPECT_TRUE(absl::IsPermissionDenied(wrong_issuer.status()));
  EXPECT_TRUE(DeleteOAuthTokens(path).ok());
}

TEST(HttpAuthConfigTest, ExpiredOAuthTokenIsRejected) {
  const std::string path = TokenPath("oauth_expired");
  HttpAuthConfig config;
  config.method = HttpAuthMethod::kOAuth;
  config.token_file = path;
  config.expected_issuer = "https://auth.example";
  OAuthTokenSet tokens = OAuthTokens(config.expected_issuer, "https://mcp.example/mcp");
  tokens.expires_at_unix_seconds = 1;
  ASSERT_TRUE(SaveOAuthTokens(path, tokens).ok());

  auto token = ResolveHttpAuthToken("https://mcp.example/mcp", config);
  ASSERT_FALSE(token.ok());
  EXPECT_TRUE(absl::IsUnauthenticated(token.status()));
  EXPECT_TRUE(DeleteOAuthTokens(path).ok());
}

TEST(HttpAuthConfigTest, MissingOAuthTokenIsRejected) {
  const std::string path = TokenPath("oauth_missing");
  std::filesystem::remove(path);
  HttpAuthConfig config;
  config.method = HttpAuthMethod::kOAuth;
  config.token_file = path;
  config.expected_issuer = "https://auth.example";

  auto token = ResolveHttpAuthToken("https://mcp.example/mcp", config);
  ASSERT_FALSE(token.ok());
  EXPECT_TRUE(absl::IsUnauthenticated(token.status()));
}

}  // namespace
}  // namespace slop::mcp
