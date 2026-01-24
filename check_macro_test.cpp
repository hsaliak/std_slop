#include "command_handler.h"
#include "database.h"
#include "http_client.h"
#include "orchestrator.h"
#include "orchestrator_openai.h"
#include "tool_executor.h"

#include "absl/log/check.h"

#include <gtest/gtest.h>

namespace slop {
namespace {

TEST(OrchestratorBuilderTest, NullDbReturnsError) {
  HttpClient client;
  auto orchestrator_or = Orchestrator::Builder(nullptr, &client).Build();
  EXPECT_FALSE(orchestrator_or.ok());
  EXPECT_EQ(orchestrator_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(OrchestratorBuilderTest, NullHttpClientReturnsError) {
  Database db;
  auto orchestrator_or = Orchestrator::Builder(&db, nullptr).Build();
  EXPECT_FALSE(orchestrator_or.ok());
  EXPECT_EQ(orchestrator_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(CheckMacroDeathTest, ToolExecutorNullDb) {
  auto executor_or = ToolExecutor::Create(nullptr);
  EXPECT_FALSE(executor_or.ok());
  EXPECT_EQ(executor_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(CheckMacroDeathTest, CommandHandlerNullDb) {
  auto handler_or = CommandHandler::Create(nullptr);
  EXPECT_FALSE(handler_or.ok());
  EXPECT_EQ(handler_or.status().code(), absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace slop
