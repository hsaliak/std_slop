#include "mcp/client/authorization.h"

#include <string>

#include "absl/status/status.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "core/json_utils.h"

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

TEST(McpAuthorizationTest, ValidatesIssuerResourceAndPkceBinding) {
  ProtectedResourceMetadata resource;
  resource.resource = "https://api.example/mcp";
  resource.authorization_servers = {"https://auth.example"};
  AuthorizationServerMetadata server;
  server.issuer = "https://auth.example";
  server.code_challenge_methods_supported = {"S256"};

  EXPECT_TRUE(ValidateAuthorizationBinding(resource, "https://api.example/mcp", server, "https://auth.example").ok());
  server.issuer = "https://attacker.example";
  EXPECT_EQ(ValidateAuthorizationBinding(resource, "https://api.example/mcp", server, "https://auth.example").code(),
            absl::StatusCode::kPermissionDenied);
  server.issuer = "https://auth.example";
  server.code_challenge_methods_supported.clear();
  EXPECT_EQ(ValidateAuthorizationBinding(resource, "https://api.example/mcp", server, "https://auth.example").code(),
            absl::StatusCode::kFailedPrecondition);
}

TEST(McpAuthorizationTest, MergesScopesWithBoundedDeduplication) {
  auto merged = MergeAuthorizationScopes({"repo", "read:user"}, {"read:user", "issues"}, 3);
  ASSERT_TRUE(merged.ok()) << merged.status();
  EXPECT_EQ(*merged, (std::vector<std::string>{"repo", "read:user", "issues"}));
  EXPECT_EQ(MergeAuthorizationScopes({"repo"}, {"issues"}, 1).status().code(), absl::StatusCode::kResourceExhausted);
  EXPECT_EQ(MergeAuthorizationScopes({""}, {}, 3).status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(McpAuthorizationTest, ParsesBoundClientIdMetadataDocument) {
  const nlohmann::json metadata = {
      {"client_id", "https://client.example/metadata.json"},
      {"redirect_uris", {"http://127.0.0.1/callback"}},
      {"token_endpoint_auth_method", "none"},
  };
  auto parsed = ParseClientIdMetadataDocument(metadata, "https://client.example/metadata.json");
  ASSERT_TRUE(parsed.ok()) << parsed.status();
  EXPECT_EQ(parsed->redirect_uris.size(), 1);
  EXPECT_EQ(ParseClientIdMetadataDocument(metadata, "https://attacker.example/metadata.json").status().code(),
            absl::StatusCode::kPermissionDenied);
}

}  // namespace
}  // namespace slop::mcp
