#include "mcp/runtime.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "core/json_utils.h"
#include "core/http_client.h"
#include "mcp/registry.h"
#include "nlohmann/json.hpp"
#include "tools/tool_executor.h"

#include <gtest/gtest.h>

namespace slop::mcp {
namespace {

class FakeHttpClient : public HttpClient {};

class FakeRuntimeSession : public RuntimeSession {
 public:
  FakeRuntimeSession(std::vector<Tool> tools, ToolCallResult result)
      : tools_(std::move(tools)), result_(std::move(result)) {}

  absl::StatusOr<std::vector<Tool>> ListTools() override { return tools_; }

  absl::StatusOr<ToolCallResult> CallTool(const std::string& name, const nlohmann::json& arguments) override {
    called_tool_name = name;
    called_arguments = arguments;
    return result_;
  }

  std::string called_tool_name;
  nlohmann::json called_arguments;

 private:
  std::vector<Tool> tools_;
  ToolCallResult result_;
};

Tool MakeTool(const std::string& name) {
  Tool tool;
  tool.name = name;
  tool.description = absl::StrCat("description for ", name);
  tool.input_schema = nlohmann::json{{"type", "object"}};
  return tool;
}

ServerRegistryEntry MakeEntry(const std::string& name) {
  ServerRegistryEntry entry;
  entry.name = name;
  entry.url = absl::StrCat("https://", name, ".example/mcp");
  entry.auth = "none";
  entry.enabled = true;
  return entry;
}

std::string TempRegistryPath() {
  return absl::StrCat(::testing::TempDir(), "/std_slop_mcp_runtime_", absl::ToUnixNanos(absl::Now()), ".ini");
}

TEST(McpRuntimeTest, DuplicateServerToolNamesDoNotCollide) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor.ok());
  FakeHttpClient http_client;

  const std::string registry_path = TempRegistryPath();
  ASSERT_TRUE(SaveServerRegistry(registry_path, {MakeEntry("github"), MakeEntry("gitlab")}).ok());

  std::vector<FakeRuntimeSession*> sessions;
  RuntimeOptions options;
  options.registry_path = registry_path;
  RuntimeManager manager(&db, executor->get(), &http_client, options,
                         [&sessions](const ServerRegistryEntry&, HttpClient*, const RuntimeOptions&) {
                           ToolCallResult result;
                           result.content.push_back(nlohmann::json{{"type", "text"}, {"text", "ok"}});
                           auto session = std::make_unique<FakeRuntimeSession>(std::vector<Tool>{MakeTool("search")}, result);
                           sessions.push_back(session.get());
                           return absl::StatusOr<std::unique_ptr<RuntimeSession>>(std::move(session));
                         });
  ASSERT_TRUE(manager.Start().ok());

  auto tools = db.GetTopLevelTools();
  ASSERT_TRUE(tools.ok());
  const auto has_tool = [&](const std::string& name) {
    return std::any_of(tools->begin(), tools->end(), [&](const Database::Tool& tool) { return tool.name == name; });
  };
  EXPECT_TRUE(has_tool("mcp_github_search"));
  EXPECT_TRUE(has_tool("mcp_gitlab_search"));

  auto output = (*executor)->Execute("mcp_github_search", nlohmann::json{{"query", "repo"}});
  ASSERT_TRUE(output.ok()) << output.status();
  EXPECT_EQ(sessions[0]->called_tool_name, "search");
  EXPECT_EQ(sessions[0]->called_arguments["query"], "repo");
}

TEST(McpRuntimeTest, ProviderUnsafeLongNamesAreRejected) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor.ok());
  FakeHttpClient http_client;

  const std::string registry_path = TempRegistryPath();
  ASSERT_TRUE(SaveServerRegistry(registry_path, {MakeEntry("github")}).ok());
  RuntimeOptions options;
  options.registry_path = registry_path;
  RuntimeManager manager(&db, executor->get(), &http_client, options,
                         [](const ServerRegistryEntry&, HttpClient*, const RuntimeOptions&) {
                           ToolCallResult result;
                           result.content.push_back(nlohmann::json{{"type", "text"}, {"text", "ok"}});
                           auto session = std::make_unique<FakeRuntimeSession>(
                               std::vector<Tool>{MakeTool("tool_name_that_is_longer_than_provider_function_name_limits")}, result);
                           return absl::StatusOr<std::unique_ptr<RuntimeSession>>(std::move(session));
                         });
  absl::Status status = manager.Start();
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(absl::IsFailedPrecondition(status));
}

TEST(McpRuntimeTest, SanitizedToolNameCollisionsAreRejected) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor.ok());
  FakeHttpClient http_client;

  const std::string registry_path = TempRegistryPath();
  ASSERT_TRUE(SaveServerRegistry(registry_path, {MakeEntry("github")}).ok());
  RuntimeOptions options;
  options.registry_path = registry_path;
  RuntimeManager manager(&db, executor->get(), &http_client, options,
                         [](const ServerRegistryEntry&, HttpClient*, const RuntimeOptions&) {
                           ToolCallResult result;
                           result.content.push_back(nlohmann::json{{"type", "text"}, {"text", "ok"}});
                           auto session = std::make_unique<FakeRuntimeSession>(
                               std::vector<Tool>{MakeTool("search/repo"), MakeTool("search_repo")}, result);
                           return absl::StatusOr<std::unique_ptr<RuntimeSession>>(std::move(session));
                         });
  absl::Status status = manager.Start();
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(absl::IsFailedPrecondition(status));
}

TEST(McpRuntimeTest, ToolErrorIsPreservedInNormalizedOutput) {
  ToolCallResult result;
  result.is_error = true;
  result.content.push_back(nlohmann::json{{"type", "text"}, {"text", "tool failed"}});
  auto normalized = NormalizeToolCallResult(result);
  ASSERT_TRUE(normalized.ok()) << normalized.status();
  auto parsed = json_parse(*normalized);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE((*parsed)["is_error"].get<bool>());
  EXPECT_EQ((*parsed)["content"][0]["text"], "tool failed");
}

TEST(McpRuntimeTest, AuthFailureDoesNotExposeStaleTool) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.RegisterTool({"mcp_github_search", "stale", "{}", true, 0, true}).ok());
  auto executor = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor.ok());
  FakeHttpClient http_client;

  const std::string registry_path = TempRegistryPath();
  ASSERT_TRUE(SaveServerRegistry(registry_path, {MakeEntry("github")}).ok());
  RuntimeOptions options;
  options.registry_path = registry_path;
  RuntimeManager manager(&db, executor->get(), &http_client, options,
                         [](const ServerRegistryEntry&, HttpClient*, const RuntimeOptions&) {
                           return absl::StatusOr<std::unique_ptr<RuntimeSession>>(
                               absl::UnauthenticatedError("login required"));
                         });
  ASSERT_TRUE(manager.Start().ok());
  auto tools = db.GetTopLevelTools();
  ASSERT_TRUE(tools.ok());
  EXPECT_EQ(std::find_if(tools->begin(), tools->end(), [](const Database::Tool& tool) {
              return tool.name == "mcp_github_search";
            }),
            tools->end());
}

TEST(McpRuntimeTest, MalformedResultContentIsRejected) {
  ToolCallResult result;
  result.content.push_back("not an object");
  auto normalized = NormalizeToolCallResult(result);
  EXPECT_FALSE(normalized.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(normalized.status()));
}

TEST(McpRuntimeTest, MalformedStructuredContentIsRejected) {
  ToolCallResult result;
  result.content.push_back(nlohmann::json{{"type", "text"}, {"text", "ok"}});
  result.structured_content = "not an object";
  auto normalized = NormalizeToolCallResult(result);
  EXPECT_FALSE(normalized.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(normalized.status()));
}

}  // namespace
}  // namespace slop::mcp
