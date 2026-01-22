#include <gtest/gtest.h>

#include "database.h"
#include "http_client.h"
#include "orchestrator_openai.h"
namespace slop {

class OpenAiOrchestratorTest : public ::testing::Test {
 protected:
  Database db;
  HttpClient http;

  void SetUp() override {
    ASSERT_TRUE(db.Init(":memory:").ok());
  }
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
    std::cout << "\n=== Payload WITH --strip-reasoning ==="  << std::endl;
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
    std::cout << "\n=== Payload WITHOUT --strip-reasoning ==="  << std::endl;
    std::cout << payload.dump(2) << std::endl;
}

}  // namespace slop
