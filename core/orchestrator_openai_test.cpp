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

TEST_F(OpenAiOrchestratorTest, AssemblePayloadIncludesStripReasoning) {
  OpenAiOrchestrator orchestrator(&db, &http, "gpt-4", "https://api.openai.com/v1");
  orchestrator.SetStripReasoning(true);

  auto result = orchestrator.AssemblePayload("s1", "System prompt", {});
  ASSERT_TRUE(result.ok());

  nlohmann::json payload = *result;
  ASSERT_TRUE(payload.contains("transforms"));
  ASSERT_TRUE(payload["transforms"].is_array());
  EXPECT_EQ(payload["transforms"][0], "strip_reasoning");
}

TEST_F(OpenAiOrchestratorTest, AssemblePayloadExcludesStripReasoningWhenDisabled) {
  OpenAiOrchestrator orchestrator(&db, &http, "gpt-4", "https://api.openai.com/v1");
  orchestrator.SetStripReasoning(false);

  auto result = orchestrator.AssemblePayload("s1", "System prompt", {});
  ASSERT_TRUE(result.ok());

  nlohmann::json payload = *result;
  EXPECT_FALSE(payload.contains("transforms"));
}

TEST_F(OpenAiOrchestratorTest, PayloadStructureWithStripReasoning) {
  // Verify the complete payload structure includes transforms
  OpenAiOrchestrator orchestrator(&db, &http, "gpt-4-turbo", "https://api.openai.com/v1");
  orchestrator.SetStripReasoning(true);

  auto result = orchestrator.AssemblePayload("session1", "You are helpful.", {});
  ASSERT_TRUE(result.ok());

  nlohmann::json payload = *result;

  // Verify core fields
  EXPECT_EQ(payload["model"], "gpt-4-turbo");
  ASSERT_TRUE(payload.contains("messages"));
  ASSERT_TRUE(payload["messages"].is_array());

  // Verify transforms field is present and correctly formatted
  ASSERT_TRUE(payload.contains("transforms"));
  ASSERT_TRUE(payload["transforms"].is_array());
  ASSERT_EQ(payload["transforms"].size(), 1);
  EXPECT_EQ(payload["transforms"][0], "strip_reasoning");

  // Print the payload for inspection
  std::cout << "\n=== Payload WITH --strip-reasoning ===" << std::endl;
  std::cout << payload.dump(2) << std::endl;
}

TEST_F(OpenAiOrchestratorTest, PayloadStructureWithoutStripReasoning) {
  // Verify the payload without transforms when disabled
  OpenAiOrchestrator orchestrator(&db, &http, "gpt-4-turbo", "https://api.openai.com/v1");
  orchestrator.SetStripReasoning(false);

  auto result = orchestrator.AssemblePayload("session1", "You are helpful.", {});
  ASSERT_TRUE(result.ok());

  nlohmann::json payload = *result;

  // Verify core fields
  EXPECT_EQ(payload["model"], "gpt-4-turbo");
  ASSERT_TRUE(payload.contains("messages"));
  ASSERT_TRUE(payload["messages"].is_array());

  // Verify transforms field is NOT present
  EXPECT_FALSE(payload.contains("transforms"));

  // Print the payload for inspection
  std::cout << "\n=== Payload WITHOUT --strip-reasoning ===" << std::endl;
  std::cout << payload.dump(2) << std::endl;
}

TEST_F(OpenAiOrchestratorTest, OpenAiProactiveFiltering) {
  OpenAiOrchestrator orchestrator(&db, &http, "gpt-4", "https://api.openai.com/v1");

  // Register only "tool1"
  ASSERT_TRUE(db.RegisterTool({"tool1", "desc1", "{}", true}).ok());

  // Add "tool1" (valid) and "tool2" (invalid) calls
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
  // Index 1: assistant (tool1 call)
  EXPECT_EQ(messages[1]["role"], "assistant");
  EXPECT_TRUE(messages[1].contains("tool_calls"));
  // Index 2: tool (tool1 response)
  EXPECT_EQ(messages[2]["role"], "tool");
  EXPECT_EQ(messages[2]["tool_call_id"], "c1");
  // Index 3: assistant (tool2 call - suppressed)
  EXPECT_EQ(messages[3]["role"], "assistant");
  EXPECT_FALSE(messages[3].contains("tool_calls"));
  EXPECT_TRUE(absl::StrContains(messages[3]["content"].get<std::string>(), "suppressed"));
  // Index 4: user (tool2 response - suppressed and role changed)
  EXPECT_EQ(messages[4]["role"], "user");
  EXPECT_TRUE(absl::StrContains(messages[4]["content"].get<std::string>(), "suppressed"));
}

}  // namespace slop
