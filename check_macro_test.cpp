#include "command_handler.h"
#include "database.h"
#include "http_client.h"
#include "orchestrator_openai.h"
#include "tool_executor.h"

#include "absl/log/check.h"

#include <gtest/gtest.h>

namespace slop {
namespace {

TEST(CheckMacroDeathTest, OpenAiOrchestratorNullDb) {
  HttpClient client;
  EXPECT_DEATH({ OpenAiOrchestrator orchestrator(nullptr, &client, "gpt-4o", ""); }, "Check failed: db_ != nullptr");
}

TEST(CheckMacroDeathTest, OpenAiOrchestratorNullHttpClient) {
  Database db;
  EXPECT_DEATH({ OpenAiOrchestrator orchestrator(&db, nullptr, "gpt-4o", ""); },
               "Check failed: http_client_ != nullptr");
}

TEST(CheckMacroDeathTest, ToolExecutorNullDb) {
  EXPECT_DEATH({ ToolExecutor executor(nullptr); }, "Check failed: db_ != nullptr");
}

TEST(CheckMacroDeathTest, CommandHandlerNullDb) {
  EXPECT_DEATH({ CommandHandler handler(nullptr); }, "Check failed: db_ != nullptr");
}

}  // namespace
}  // namespace slop
