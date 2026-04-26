#include "rpc/execution_policy.h"

#include <gtest/gtest.h>

#include "absl/status/status.h"
#include "core/database.h"
#include "nlohmann/json.hpp"

namespace slop::rpc::v1 {
namespace {

TEST(ExecutionPolicyTest, DisablesAskUserWhenPolicyRequiresNonInteractive) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = slop::ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok()) << executor_or.status();

  ServerRuntimeConfig server_config;
  server_config.disable_ask_user = true;

  ApplyServerExecutionPolicy(**executor_or, server_config);

  auto res = (*executor_or)->Execute("ask_user", nlohmann::json::object());
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kPermissionDenied);
}

TEST(ExecutionPolicyTest, EnforcesSingleSubqueryDepthLimit) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = slop::ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok()) << executor_or.status();

  ServerRuntimeConfig server_config;
  ApplyServerExecutionPolicy(**executor_or, server_config);

  (*executor_or)->SetExecutionContext(slop::ToolExecutor::ExecutionScope::kSubquery, 1);
  auto allowed = (*executor_or)->Execute("describe_db", nlohmann::json::object());
  ASSERT_TRUE(allowed.ok()) << allowed.status();

  (*executor_or)->SetExecutionContext(slop::ToolExecutor::ExecutionScope::kSubquery, 2);
  auto blocked = (*executor_or)->Execute("describe_db", nlohmann::json::object());
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(ExecutionPolicyTest, AppliesPolicyToInteractionEngineConfig) {
  ServerRuntimeConfig server_config;
  server_config.disable_ask_user = true;

  slop::InteractionEngine::Config config;

  ApplyServerExecutionPolicy(config, server_config);

  EXPECT_FALSE(config.allow_ask_user);
  EXPECT_EQ(config.max_subquery_execution_depth, 1);
}

}  // namespace
}  // namespace slop::rpc::v1
