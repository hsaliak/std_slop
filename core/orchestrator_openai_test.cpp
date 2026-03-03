#include "core/orchestrator_openai.h"

#include "absl/strings/match.h"

#include "core/database.h"
#include "core/http_client.h"

#include <gtest/gtest.h>
namespace slop {

class OpenAiOrchestratorTest : public ::testing::Test {
 protected:
  Database db;
  HttpClient http;

  void SetUp() override { ASSERT_TRUE(db.Init(":memory:").ok()); }
};

TEST_F(OpenAiOrchestratorTest, PayloadStructureBasic) {
  OpenAiOrchestrator orchestrator(&db, &http, "gpt-4-turbo", "https://api.openai.com/v1");

  auto result = orchestrator.AssemblePayload("session1", "You are helpful.", {});
  ASSERT_TRUE(result.ok());

  nlohmann::json payload = *result;

  // Verify core fields
  EXPECT_EQ(payload["model"], "gpt-4-turbo");
  ASSERT_TRUE(payload.contains("messages"));
  ASSERT_TRUE(payload["messages"].is_array());
}

TEST_F(OpenAiOrchestratorTest, OpenAiProactiveFiltering) {
  OpenAiOrchestrator orchestrator(&db, &http, "gpt-4", "https://api.openai.com/v1");

  // Only run_js is exposed at top-level. tool1 is preserved; tool2 is filtered.
  ASSERT_TRUE(db.RegisterTool({"tool1", "desc1", "{}", true}).ok());

  // Add "tool1" (preserved) and "tool2" (suppressed) calls.
  nlohmann::json tool_call1 = {
      {"role", "assistant"},
      {"tool_calls", {{{"id", "c1"}, {"type", "function"}, {"function", {{"name", "tool1"}, {"arguments", "{}"}}}}}}};
  nlohmann::json tool_call2 = {
      {"role", "assistant"},
      {"tool_calls", {{{"id", "c2"}, {"type", "function"}, {"function", {{"name", "tool2"}, {"arguments", "{}"}}}}}}};

  ASSERT_TRUE(db.AppendMessage("s1", "assistant", tool_call1.dump(), "c1|tool1", "tool_call").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "tool", "res1", "c1|tool1", "completed").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", tool_call2.dump(), "c2|tool2", "tool_call").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "tool", "res2", "c2|tool2", "completed").ok());

  auto history_or = db.GetConversationHistory("s1", false);
  ASSERT_TRUE(history_or.ok());

  auto result = orchestrator.AssemblePayload("s1", "System prompt", *history_or);
  ASSERT_TRUE(result.ok());

  nlohmann::json payload = *result;
  nlohmann::json messages = payload["messages"];

  // Index 0: system
  EXPECT_EQ(messages[0]["role"], "system");
  // Index 1: assistant (tool1 call preserved)
  EXPECT_EQ(messages[1]["role"], "assistant");
  EXPECT_TRUE(messages[1].contains("tool_calls"));
  // Index 2: tool (tool1 response preserved)
  EXPECT_EQ(messages[2]["role"], "tool");
  EXPECT_EQ(messages[2]["tool_call_id"], "c1");
  // Index 3: assistant (tool2 call suppressed)
  EXPECT_EQ(messages[3]["role"], "assistant");
  EXPECT_FALSE(messages[3].contains("tool_calls"));
  EXPECT_TRUE(absl::StrContains(messages[3]["content"].get<std::string>(), "suppressed"));
  // Index 4: user (tool2 response suppressed)
  EXPECT_EQ(messages[4]["role"], "user");
  EXPECT_TRUE(absl::StrContains(messages[4]["content"].get<std::string>(), "suppressed"));
}

}  // namespace slop



