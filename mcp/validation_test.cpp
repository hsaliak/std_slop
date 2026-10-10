#include "mcp/validation.h"

#include <string>

#include "gtest/gtest.h"

namespace slop::mcp {
namespace {

TEST(ValidationTest, AcceptsValidHttpUrls) {
  for (const std::string& url : {"https://example.com/mcp", "http://127.0.0.1:8123/mcp", "http://[::1]:8123/mcp",
                                 "http://[::ffff:192.0.2.1]/mcp", "https://example.com/%2F?q=ok",
                                 "https://foo-bar.example/path?query=a?b&x=y", "https://localhost/mcp"}) {
    EXPECT_TRUE(IsValidHttpUrl(url, 4096)) << url;
  }
  EXPECT_TRUE(IsValidHttpUrl("https://example.com/mcp", 2048, true));
}

TEST(ValidationTest, RejectsMalformedOrUnsafeHttpUrls) {
  for (const std::string& url : {"",
                                 "/mcp",
                                 "ftp://example.com",
                                 "https:///mcp",
                                 "https://user@example.com/mcp",
                                 "https://example.com/#fragment",
                                 "https://example.com/%ZZ",
                                 "https://./mcp",
                                 "https://-bad.example/mcp",
                                 "https://bad-.example/mcp",
                                 "https://a..b/mcp",
                                 "http://:80/mcp",
                                 "http://example.com:65536/mcp",
                                 "http://127.000.0.1/mcp",
                                 "http://256.1.1.1/mcp",
                                 "http://[not-ipv6]/mcp",
                                 "http://[::1/mcp",
                                 "http://example.com/%",
                                 "https://example.com/a|b",
                                 "https://example.com/{x}",
                                 "https://example.com/a\\\\b"}) {
    EXPECT_FALSE(IsValidHttpUrl(url, 4096)) << url;
  }
  EXPECT_FALSE(IsValidHttpUrl("http://127.0.0.1/mcp", 4096, true));
  EXPECT_FALSE(IsValidHttpUrl("https://example.com/mcp", 10));
  EXPECT_FALSE(IsValidHttpUrl(std::string("https://example.com/\0mcp", 24), 4096));
}

TEST(ValidationTest, ValidatesAbsolutePaths) {
  EXPECT_TRUE(IsValidAbsolutePath("/tmp/token.json", 4096));
  EXPECT_TRUE(IsValidAbsolutePath("/" + std::string(4095, 'a'), 4096));
  EXPECT_FALSE(IsValidAbsolutePath("", 4096));
  EXPECT_FALSE(IsValidAbsolutePath("relative/token.json", 4096));
  EXPECT_FALSE(IsValidAbsolutePath("/tmp/with\nnewline", 4096));
  EXPECT_FALSE(IsValidAbsolutePath(std::string("/tmp/with\0nul", 13), 4096));
  EXPECT_FALSE(IsValidAbsolutePath("/" + std::string(4096, 'a'), 4096));
}

TEST(ValidationTest, ValidatesEnvironmentVariableNames) {
  EXPECT_TRUE(IsValidEnvironmentVariableName("TOKEN"));
  EXPECT_TRUE(IsValidEnvironmentVariableName("_TOKEN_1"));
  EXPECT_FALSE(IsValidEnvironmentVariableName(""));
  EXPECT_FALSE(IsValidEnvironmentVariableName("9TOKEN"));
  EXPECT_FALSE(IsValidEnvironmentVariableName("TOKEN-NAME"));
  EXPECT_FALSE(IsValidEnvironmentVariableName("TOKEN=VALUE"));
  EXPECT_FALSE(IsValidEnvironmentVariableName("TOKEN", 4));
}

TEST(ValidationTest, ValidatesMcpServerNames) {
  EXPECT_TRUE(IsValidMcpServerName("docs-v2"));
  EXPECT_TRUE(IsValidMcpServerName("local_server"));
  for (const std::string& name : {"", "bad name", "path/name", "dot.name", "bad\\nname"}) {
    EXPECT_FALSE(IsValidMcpServerName(name)) << name;
  }
}

TEST(ValidationTest, ValidatesGatewayJavaScriptAliases) {
  for (const std::string& alias : {"docs", "_private", "server1", "server$1"}) {
    EXPECT_TRUE(IsSafeGatewayServerAlias(alias)) << alias;
  }
  for (const std::string& alias : {"", "bad-name", "1server", "await", "mcp", "constructor", "__proto__", "toString"}) {
    EXPECT_FALSE(IsSafeGatewayServerAlias(alias)) << alias;
  }
  EXPECT_FALSE(IsSafeGatewayServerAlias(std::string(65, 'a')));
}

TEST(ValidationTest, ValidatesObjectFields) {
  EXPECT_TRUE(ValidateJsonObjectFields({{"name", "docs"}}, {"name", "auth"}, "server").ok());
  EXPECT_TRUE(ValidateJsonObjectFields(nlohmann::json::object(), {"name"}, "server").ok());

  const absl::Status unknown = ValidateJsonObjectFields({{"extra", true}}, {"name"}, "server");
  EXPECT_TRUE(absl::IsInvalidArgument(unknown));
  EXPECT_NE(unknown.message().find("extra"), std::string::npos);
  EXPECT_TRUE(absl::IsInvalidArgument(ValidateJsonObjectFields(nlohmann::json::array(), {"name"}, "server")));
}

}  // namespace
}  // namespace slop::mcp
