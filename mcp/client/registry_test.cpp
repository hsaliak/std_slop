#include "mcp/client/registry.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

#include "gtest/gtest.h"

namespace slop::mcp {
namespace {

std::string TestRegistryPath() {
  return (std::filesystem::temp_directory_path() / "slop_mcp_registry_test.ini").string();
}

ServerRegistryEntry Entry(std::string name, std::string url, std::string auth = kAuthNone) {
  ServerRegistryEntry entry;
  entry.name = std::move(name);
  entry.url = std::move(url);
  entry.auth = std::move(auth);
  entry.enabled = true;
  return entry;
}

TEST(RegistryTest, UpsertsLoadsAndRemovesServer) {
  const std::string path = TestRegistryPath();
  std::filesystem::remove(path);
  ServerRegistryEntry entry = Entry("github", "https://example.com/mcp", kAuthOAuth);
  entry.scopes = {"repo"};
  entry.client_id = "client-id";
  entry.authorization_endpoint = "https://auth.example/authorize";
  entry.token_endpoint = "https://auth.example/token";
  ASSERT_TRUE(UpsertServerRegistryEntry(path, entry).ok());

  auto entries = LoadServerRegistry(path);
  ASSERT_TRUE(entries.ok());
  ASSERT_EQ(entries->size(), 1);
  EXPECT_EQ((*entries)[0].name, "github");
  EXPECT_EQ((*entries)[0].scopes, std::vector<std::string>({"repo"}));

  ASSERT_TRUE(RemoveServerRegistryEntry(path, "github").ok());
  entries = LoadServerRegistry(path);
  ASSERT_TRUE(entries.ok());
  EXPECT_TRUE(entries->empty());
  std::filesystem::remove(path);
}

TEST(RegistryTest, StdioArgumentsRoundTripWithoutExpansionOrIniCommentLoss) {
  const std::string path = TestRegistryPath();
  std::filesystem::remove(path);
  ServerRegistryEntry entry = Entry("local", "");
  entry.transport = kTransportStdio;
  entry.command = "/absolute/path/to/server";
  entry.args = {"two words", "$MCP_REGISTRY_TEST #; value", "quote \" value", "line\nbreak"};
  setenv("MCP_REGISTRY_TEST", "expanded", 1);
  ASSERT_TRUE(UpsertServerRegistryEntry(path, entry).ok());

  auto entries = LoadServerRegistry(path);
  ASSERT_TRUE(entries.ok()) << entries.status();
  ASSERT_EQ(entries->size(), 1);
  EXPECT_EQ((*entries)[0].transport, kTransportStdio);
  EXPECT_EQ((*entries)[0].command, entry.command);
  EXPECT_EQ((*entries)[0].args, entry.args);
  EXPECT_TRUE((*entries)[0].url.empty());
  EXPECT_EQ((*entries)[0].auth, kAuthNone);
  std::filesystem::remove(path);
  unsetenv("MCP_REGISTRY_TEST");
}

TEST(RegistryTest, MissingStdioArgsJsonDefaultsToEmpty) {
  const std::string path = TestRegistryPath();
  {
    std::ofstream file(path);
    file << "[server.local]\ntransport = stdio\ncommand = /path/server\nenabled = true\n";
  }
  auto entries = LoadServerRegistry(path);
  ASSERT_TRUE(entries.ok()) << entries.status();
  ASSERT_EQ(entries->size(), 1);
  EXPECT_TRUE((*entries)[0].args.empty());
  std::filesystem::remove(path);
}

TEST(RegistryTest, ParsesAndValidatesStdioArgsJson) {
  auto args = ParseServerArgsJson(R"(["one", "two words", "$HOME", "#", ";"])");
  ASSERT_TRUE(args.ok()) << args.status();
  EXPECT_EQ(*args, std::vector<std::string>({"one", "two words", "$HOME", "#", ";"}));
  EXPECT_FALSE(ParseServerArgsJson("{").ok());
  EXPECT_FALSE(ParseServerArgsJson(R"({"arg":"value"})").ok());
  EXPECT_FALSE(ParseServerArgsJson(R"(["valid", 3])").ok());
}

TEST(RegistryTest, RejectsTransportSpecificFieldsOnWrongTransport) {
  ServerRegistryEntry stdio = Entry("local", "");
  stdio.transport = kTransportStdio;
  stdio.command = "/path/server";
  stdio.url = "https://example.com/mcp";
  EXPECT_FALSE(ValidateServerRegistryEntry(stdio).ok());
  stdio.url.clear();
  stdio.auth = kAuthBearer;
  EXPECT_FALSE(ValidateServerRegistryEntry(stdio).ok());

  ServerRegistryEntry http = Entry("remote", "https://example.com/mcp");
  http.command = "/path/server";
  EXPECT_FALSE(ValidateServerRegistryEntry(http).ok());
  http.command.clear();
  http.args = {"arg"};
  EXPECT_FALSE(ValidateServerRegistryEntry(http).ok());
}

TEST(RegistryTest, RejectsInvalidServerEntry) {
  ServerRegistryEntry invalid_name = Entry("bad name", "https://example.com/mcp");
  ServerRegistryEntry invalid_url = Entry("server", "ftp://example.com/mcp");
  ServerRegistryEntry missing_bearer_path = Entry("server", "https://example.com/mcp", kAuthBearer);
  EXPECT_FALSE(ValidateServerRegistryEntry(invalid_name).ok());
  EXPECT_FALSE(ValidateServerRegistryEntry(invalid_url).ok());
  EXPECT_FALSE(ValidateServerRegistryEntry(missing_bearer_path).ok());
}

TEST(RegistryTest, RejectsIniInjection) {
  ServerRegistryEntry entry = Entry("server", "https://example.com/mcp\n[server.injected]");
  EXPECT_FALSE(ValidateServerRegistryEntry(entry).ok());
}

TEST(RegistryTest, RejectsDuplicateNamesWhenSaving) {
  const std::string path = TestRegistryPath();
  std::filesystem::remove(path);
  const ServerRegistryEntry first = Entry("server", "https://one.example/mcp");
  const ServerRegistryEntry second = Entry("server", "https://two.example/mcp");
  EXPECT_FALSE(SaveServerRegistry(path, {first, second}).ok());
  std::filesystem::remove(path);
}

}  // namespace
}  // namespace slop::mcp
