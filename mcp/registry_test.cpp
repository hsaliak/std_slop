#include "mcp/registry.h"

#include <filesystem>
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
