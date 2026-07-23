#include "mcp/registry.h"

#include <filesystem>

#include "gtest/gtest.h"

namespace slop::mcp {
namespace {

std::string TestRegistryPath() {
  return (std::filesystem::temp_directory_path() / "slop_mcp_registry_test.ini").string();
}

TEST(RegistryTest, UpsertsLoadsAndRemovesServer) {
  const std::string path = TestRegistryPath();
  std::filesystem::remove(path);
  const ServerRegistryEntry entry{"github", "https://example.com/mcp", "oauth", true, {"repo"}, ""};
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
  ServerRegistryEntry invalid_name{"bad name", "https://example.com/mcp", "none", true, {}, ""};
  ServerRegistryEntry invalid_url{"server", "ftp://example.com/mcp", "none", true, {}, ""};
  ServerRegistryEntry missing_bearer_path{"server", "https://example.com/mcp", "bearer", true, {}, ""};
  EXPECT_FALSE(ValidateServerRegistryEntry(invalid_name).ok());
  EXPECT_FALSE(ValidateServerRegistryEntry(invalid_url).ok());
  EXPECT_FALSE(ValidateServerRegistryEntry(missing_bearer_path).ok());
}

}  // namespace
}  // namespace slop::mcp
