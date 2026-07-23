#include "app/mcp_commands.h"

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "core/http_client.h"
#include "mcp/registry.h"

#include <gtest/gtest.h>

namespace slop {
namespace {

class ScopedHome {
 public:
  ScopedHome() {
    const char* home = std::getenv("HOME");
    if (home != nullptr) old_home_ = home;
    path_ = absl::StrCat(::testing::TempDir(), "/std_slop_mcp_commands_", absl::ToUnixNanos(absl::Now()));
    setenv("HOME", path_.c_str(), 1);
  }

  ~ScopedHome() {
    if (old_home_.empty()) {
      unsetenv("HOME");
    } else {
      setenv("HOME", old_home_.c_str(), 1);
    }
  }

 private:
  std::string old_home_;
  std::string path_;
};

TEST(McpCommandsTest, AddListAndRemoveRegistryEntry) {
  ScopedHome home;
  HttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status = RunMcpCommand({"mcp", "add", "github", "--url", "https://example.com/mcp", "--auth", "none"},
                                      &http_client, &input, &output, &error);
  ASSERT_TRUE(status.ok()) << status;
  EXPECT_NE(output.str().find("MCP server saved: github"), std::string::npos);

  auto entries = mcp::LoadServerRegistry(mcp::DefaultRegistryPath());
  ASSERT_TRUE(entries.ok()) << entries.status();
  ASSERT_EQ(entries->size(), 1);
  EXPECT_EQ((*entries)[0].name, "github");
  EXPECT_EQ((*entries)[0].url, "https://example.com/mcp");

  output.str("");
  output.clear();
  status = RunMcpCommand({"mcp", "list"}, &http_client, &input, &output, &error);
  ASSERT_TRUE(status.ok()) << status;
  EXPECT_NE(output.str().find("github\tnone\tenabled\thttps://example.com/mcp"), std::string::npos);

  output.str("");
  output.clear();
  status = RunMcpCommand({"mcp", "remove", "github"}, &http_client, &input, &output, &error);
  ASSERT_TRUE(status.ok()) << status;
  EXPECT_NE(output.str().find("MCP server removed: github"), std::string::npos);
  entries = mcp::LoadServerRegistry(mcp::DefaultRegistryPath());
  ASSERT_TRUE(entries.ok()) << entries.status();
  EXPECT_TRUE(entries->empty());
}

TEST(McpCommandsTest, AddRejectsDuplicateFlag) {
  ScopedHome home;
  HttpClient http_client;
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream error;

  absl::Status status = RunMcpCommand({"mcp", "add", "github", "--url", "https://one.example/mcp", "--url",
                                       "https://two.example/mcp"},
                                      &http_client, &input, &output, &error);
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(status));
}

}  // namespace
}  // namespace slop
