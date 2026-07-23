#include "mcp/authorization.h"

#include <string>

#include "absl/status/status.h"
#include "core/json_utils.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace slop::mcp {
namespace {

TEST(McpAuthorizationTest, ParsesResourceMetadataFromWwwAuthenticate) {
  auto parsed = ParseWwwAuthenticateResourceMetadata(
      R"(Bearer realm="mcp", resource_metadata="https://example.com/.well-known/oauth-protected-resource")");

  ASSERT_TRUE(parsed.ok()) << parsed.status();
  EXPECT_EQ(*parsed, "https://example.com/.well-known/oauth-protected-resource");
}

TEST(McpAuthorizationTest, ParsesUnquotedResourceMetadata) {
  auto parsed = ParseWwwAuthenticateResourceMetadata(
      "Bearer resource_metadata=https://example.com/resource, error=invalid_token");

  ASSERT_TRUE(parsed.ok()) << parsed.status();
  EXPECT_EQ(*parsed, "https://example.com/resource");
}

TEST(McpAuthorizationTest, RejectsNonBearerWwwAuthenticate) {
  auto parsed = ParseWwwAuthenticateResourceMetadata("Basic realm=example");

  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(McpAuthorizationTest, MissingResourceMetadataIsUnauthenticated) {
  auto parsed = ParseWwwAuthenticateResourceMetadata("Bearer realm=example");

  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.status().code(), absl::StatusCode::kUnauthenticated);
}

TEST(McpAuthorizationTest, RejectsUnterminatedQuotedValue) {
  auto parsed = ParseWwwAuthenticateResourceMetadata("Bearer resource_metadata=\"https://example.com");

  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(McpAuthorizationTest, ParsesProtectedResourceMetadata) {
  const nlohmann::json metadata = {{"resource", "https://api.example.com/mcp"},
                                   {"authorization_servers", {"https://auth.example.com"}},
                                   {"scopes_supported", {"read", "write"}}};

  auto parsed = ParseProtectedResourceMetadata(metadata);

  ASSERT_TRUE(parsed.ok()) << parsed.status();
  EXPECT_EQ(parsed->resource, "https://api.example.com/mcp");
  ASSERT_EQ(parsed->authorization_servers.size(), 1);
  EXPECT_EQ(parsed->authorization_servers[0], "https://auth.example.com");
  ASSERT_EQ(parsed->scopes_supported.size(), 2);
  EXPECT_EQ(parsed->scopes_supported[0], "read");
}

TEST(McpAuthorizationTest, RejectsProtectedResourceMetadataMissingAuthServers) {
  const nlohmann::json metadata = {{"resource", "https://api.example.com/mcp"}};

  auto parsed = ParseProtectedResourceMetadata(metadata);

  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(McpAuthorizationTest, RejectsProtectedResourceMetadataBadScopesShape) {
  const nlohmann::json metadata = {{"resource", "https://api.example.com/mcp"},
                                   {"authorization_servers", {"https://auth.example.com"}},
                                   {"scopes_supported", "read"}};

  auto parsed = ParseProtectedResourceMetadata(metadata);

  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(McpAuthorizationTest, ParsesAuthorizationServerMetadata) {
  const nlohmann::json metadata = {{"issuer", "https://auth.example.com"},
                                   {"authorization_endpoint", "https://auth.example.com/authorize"},
                                   {"token_endpoint", "https://auth.example.com/token"},
                                   {"scopes_supported", {"read"}}};

  auto parsed = ParseAuthorizationServerMetadata(metadata);

  ASSERT_TRUE(parsed.ok()) << parsed.status();
  EXPECT_EQ(parsed->issuer, "https://auth.example.com");
  EXPECT_EQ(parsed->authorization_endpoint, "https://auth.example.com/authorize");
  EXPECT_EQ(parsed->token_endpoint, "https://auth.example.com/token");
  ASSERT_EQ(parsed->scopes_supported.size(), 1);
  EXPECT_EQ(parsed->scopes_supported[0], "read");
}

TEST(McpAuthorizationTest, RejectsAuthorizationServerMetadataMissingTokenEndpoint) {
  const nlohmann::json metadata = {{"issuer", "https://auth.example.com"},
                                   {"authorization_endpoint", "https://auth.example.com/authorize"}};

  auto parsed = ParseAuthorizationServerMetadata(metadata);

  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.status().code(), absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace slop::mcp
