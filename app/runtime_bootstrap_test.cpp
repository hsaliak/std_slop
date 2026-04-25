#include "app/runtime_bootstrap.h"

#include <memory>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/database.h"
#include "core/http_client.h"
#include "core/oauth_handler.h"
#include "tools/tool_dispatcher.h"

namespace slop {
namespace {

TEST(RuntimeBootstrapTest, RejectsNullDatabase) {
  HttpClient http_client;
  RuntimeBootstrapOptions options;
  options.google_api_key = "key";

  auto status_or = BootstrapRuntime(
      nullptr, &http_client, options,
      [](std::shared_ptr<OAuthHandler>*) {},
      [](ToolExecutor&) -> std::vector<std::string> { return {}; },
      [](CommandHandler&) {});

  ASSERT_FALSE(status_or.ok());
  EXPECT_EQ(status_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(RuntimeBootstrapTest, RejectsNullHttpClient) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  RuntimeBootstrapOptions options;
  options.google_api_key = "key";

  auto status_or = BootstrapRuntime(
      &db, nullptr, options,
      [](std::shared_ptr<OAuthHandler>*) {},
      [](ToolExecutor&) -> std::vector<std::string> { return {}; },
      [](CommandHandler&) {});

  ASSERT_FALSE(status_or.ok());
  EXPECT_EQ(status_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(RuntimeBootstrapTest, RejectsMissingAuth) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  HttpClient http_client;
  RuntimeBootstrapOptions options;

  auto status_or = BootstrapRuntime(
      &db, &http_client, options,
      [](std::shared_ptr<OAuthHandler>*) {},
      [](ToolExecutor&) -> std::vector<std::string> { return {}; },
      [](CommandHandler&) {});

  ASSERT_FALSE(status_or.ok());
  EXPECT_EQ(status_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(RuntimeBootstrapTest, BuildsRuntimeForGeminiPath) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  HttpClient http_client;

  RuntimeBootstrapOptions options;
  options.google_api_key = "dummy-key";
  options.model = "gemini-3-flash-preview";

  auto runtime_or = BootstrapRuntime(
      &db, &http_client, options,
      [](std::shared_ptr<OAuthHandler>*) {},
      [](ToolExecutor& tool_executor) -> std::vector<std::string> { return tool_executor.GetActiveSkills(); },
      [](CommandHandler&) {});

  ASSERT_TRUE(runtime_or.ok()) << runtime_or.status();
  EXPECT_NE(runtime_or->orchestrator, nullptr);
  EXPECT_NE(runtime_or->tool_executor, nullptr);
  EXPECT_NE(runtime_or->command_handler, nullptr);
  EXPECT_NE(runtime_or->tool_executor->dispatcher(), nullptr);
  EXPECT_NE(runtime_or->engine, nullptr);
  ASSERT_NE(runtime_or->engine_config, nullptr);
  EXPECT_FALSE(runtime_or->engine_config->openai_oauth);
}

TEST(RuntimeBootstrapTest, DispatcherCallbackRemainsValidAfterRuntimeMove) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.RegisterTool({"regression_tool", "test tool", "{}", true}).ok());
  HttpClient http_client;

  RuntimeBootstrapOptions options;
  options.google_api_key = "dummy-key";
  options.model = "gemini-3-flash-preview";

  auto runtime_or = BootstrapRuntime(
      &db, &http_client, options,
      [](std::shared_ptr<OAuthHandler>*) {},
      [](ToolExecutor& tool_executor) -> std::vector<std::string> { return tool_executor.GetActiveSkills(); },
      [](CommandHandler&) {});
  ASSERT_TRUE(runtime_or.ok()) << runtime_or.status();

  RuntimeBootstrap runtime = std::move(*runtime_or);
  runtime.tool_executor->RegisterTool(
      "regression_tool", [](const nlohmann::json&, std::shared_ptr<CancellationRequest>) -> absl::StatusOr<std::string> {
        return "callback-ok";
      });

  std::vector<ToolDispatcher::Result> results = runtime.tool_executor->dispatcher()->Dispatch(
      {{"call-1", "regression_tool", nlohmann::json::object()}}, nullptr);

  ASSERT_EQ(results.size(), 1);
  ASSERT_TRUE(results[0].output.ok()) << results[0].output.status();
  EXPECT_EQ(*results[0].output, "callback-ok");
}

}  // namespace
}  // namespace slop
