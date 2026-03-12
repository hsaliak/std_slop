#include "core/orchestrator_openai_responses.h"

#include <set>

#include "absl/status/status.h"

#include "core/database.h"
#include "core/http_client.h"
#include "core/json_utils.h"

#include <gtest/gtest.h>

namespace slop {

class OpenAiResponsesOrchestratorTest : public ::testing::Test {
 protected:
  Database db;
  HttpClient http;

  void SetUp() override { ASSERT_TRUE(db.Init(":memory:").ok()); }
};

TEST_F(OpenAiResponsesOrchestratorTest, AssemblePayloadBuildsInputAndTools) {
  ASSERT_TRUE(db.RegisterTool({"query_db", "Query database",
                               R"({"type":"object","properties":{"sql":{"type":"string"}},"required":["sql"]})", true})
                  .ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());

  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-4o", "https://api.openai.com/v1");
  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());
  auto payload_or = orchestrator.AssemblePayload("s1", "System prompt", *history_or);
  ASSERT_TRUE(payload_or.ok());

  const auto& payload = *payload_or;
  EXPECT_EQ(json_get_or(payload, "model", std::string{}), "gpt-4o");
  EXPECT_EQ(json_get_or(payload, "instructions", std::string{}), "System prompt");
  EXPECT_EQ(json_get_or(payload, "store", true), false);
  ASSERT_TRUE(payload.contains("input"));
  ASSERT_TRUE(payload["input"].is_array());
  ASSERT_TRUE(payload.contains("tools"));
  ASSERT_TRUE(payload["tools"].is_array());
  std::set<std::string> tool_names;
  for (const auto& tool : payload["tools"]) {
    tool_names.insert(json_get_or(tool, "name", std::string{}));
  }
  EXPECT_TRUE(tool_names.find("query_db") != tool_names.end());
  EXPECT_TRUE(tool_names.find("llm_query") != tool_names.end());
  EXPECT_TRUE(tool_names.find("ask_user") != tool_names.end());
}

TEST_F(OpenAiResponsesOrchestratorTest, AssemblePayloadUsesCodexInstructionsAndReasoning) {
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello codex").ok());

  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-5.3-codex", "https://chatgpt.com/backend-api/codex");
  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());
  auto payload_or = orchestrator.AssemblePayload("s1", "Codex system instructions", *history_or);
  ASSERT_TRUE(payload_or.ok());

  const auto& payload = *payload_or;
  EXPECT_EQ(json_get_or(payload, "instructions", std::string{}), "Codex system instructions");
  EXPECT_EQ(json_get_or(payload, "store", true), false);
  EXPECT_EQ(json_get_or(payload, "stream", false), true);
  EXPECT_TRUE(payload.contains("reasoning"));
  EXPECT_EQ(json_get_or(payload["reasoning"], "effort", std::string{}), "medium");
  EXPECT_EQ(json_get_or(payload["reasoning"], "summary", std::string{}), "auto");
}

TEST_F(OpenAiResponsesOrchestratorTest, AssemblePayloadUsesReasoningFromModelSuffix) {
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello codex").ok());

  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-5.3-codex:low", "https://chatgpt.com/backend-api/codex");
  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());
  auto payload_or = orchestrator.AssemblePayload("s1", "Codex system instructions", *history_or);
  ASSERT_TRUE(payload_or.ok());

  const auto& payload = *payload_or;
  EXPECT_EQ(json_get_or(payload, "model", std::string{}), "gpt-5.3-codex");
  EXPECT_TRUE(payload.contains("reasoning"));
  EXPECT_EQ(json_get_or(payload["reasoning"], "effort", std::string{}), "low");
}

TEST_F(OpenAiResponsesOrchestratorTest, AssemblePayloadRejectsInvalidReasoningSuffix) {
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello codex").ok());

  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-5.3-codex:ultra", "https://chatgpt.com/backend-api/codex");
  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());
  auto payload_or = orchestrator.AssemblePayload("s1", "Codex system instructions", *history_or);
  ASSERT_FALSE(payload_or.ok());
  EXPECT_EQ(payload_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(OpenAiResponsesOrchestratorTest, ProcessResponseParsesSsePayload) {
  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-5.3-codex", "https://chatgpt.com/backend-api/codex");
  const std::string sse_payload =
      "event: response.output_item.done\n"
      "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"Hello from SSE\"}]}}\n\n"
      "event: response.completed\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp1\","
      "\"usage\":{\"input_tokens\":2,\"output_tokens\":3}}}\n\n";

  auto st_or = orchestrator.ProcessResponse("s1", sse_payload, "g1");
  ASSERT_TRUE(st_or.ok());
  EXPECT_EQ(*st_or, 5);

  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());
  ASSERT_EQ(history_or->size(), 1);
  EXPECT_EQ((*history_or)[0].role, "assistant");
  EXPECT_EQ((*history_or)[0].content, "Hello from SSE");
}

TEST_F(OpenAiResponsesOrchestratorTest, ProcessResponseParsesSseTextDeltas) {
  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-oss-120b", "https://chatgpt.com/backend-api/codex");
  const std::string sse_payload =
      "event: response.created\n"
      "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp1\"}}\n\n"
      "event: response.output_text.delta\n"
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Hello\"}\n\n"
      "event: response.output_text.delta\n"
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\" world\"}\n\n"
      "event: response.completed\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp1\","
      "\"usage\":{\"input_tokens\":1,\"output_tokens\":2}}}\n\n";

  auto st_or = orchestrator.ProcessResponse("s1", sse_payload, "g1");
  ASSERT_TRUE(st_or.ok());
  EXPECT_EQ(*st_or, 3);

  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());
  ASSERT_EQ(history_or->size(), 1);
  EXPECT_EQ((*history_or)[0].role, "assistant");
  EXPECT_EQ((*history_or)[0].content, "Hello world");
}

TEST_F(OpenAiResponsesOrchestratorTest, ProcessResponseDoesNotDuplicateSseAssistantText) {
  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-oss-120b", "https://chatgpt.com/backend-api/codex");
  const std::string sse_payload =
      "event: response.output_text.delta\n"
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Hello\"}\n\n"
      "event: response.output_text.delta\n"
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\" world\"}\n\n"
      "event: response.output_item.done\n"
      "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"Hello world\"}]}}\n\n"
      "event: response.completed\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp1\","
      "\"usage\":{\"input_tokens\":1,\"output_tokens\":2}}}\n\n";

  auto st_or = orchestrator.ProcessResponse("s1", sse_payload, "g1");
  ASSERT_TRUE(st_or.ok());
  EXPECT_EQ(*st_or, 3);

  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());
  ASSERT_EQ(history_or->size(), 1);
  EXPECT_EQ((*history_or)[0].role, "assistant");
  EXPECT_EQ((*history_or)[0].content, "Hello world");
}

TEST_F(OpenAiResponsesOrchestratorTest, ProcessResponseDedupesSseFunctionCallItems) {
  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-5.3-codex", "https://chatgpt.com/backend-api/codex");
  const std::string sse_payload =
      "event: response.output_item.added\n"
      "data: {\"type\":\"response.output_item.added\",\"item\":{\"type\":\"function_call\",\"call_id\":\"call_1\","
      "\"name\":\"query_db\",\"arguments\":\"\"}}\n\n"
      "event: response.output_item.done\n"
      "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"function_call\",\"call_id\":\"call_1\","
      "\"name\":\"query_db\",\"arguments\":\"{\\\"sql\\\":\\\"SELECT 7\\\"}\"}}\n\n"
      "event: response.completed\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp1\"}}\n\n";

  auto st_or = orchestrator.ProcessResponse("s1", sse_payload, "g1");
  ASSERT_TRUE(st_or.ok());

  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());
  ASSERT_EQ(history_or->size(), 1);
  EXPECT_EQ((*history_or)[0].status, "tool_call");

  auto calls_or = orchestrator.ParseToolCalls((*history_or)[0]);
  ASSERT_TRUE(calls_or.ok());
  ASSERT_EQ(calls_or->size(), 1);
  EXPECT_EQ((*calls_or)[0].id, "call_1");
  EXPECT_EQ((*calls_or)[0].name, "query_db");
  EXPECT_EQ(json_get_or((*calls_or)[0].args, "sql", std::string{}), "SELECT 7");
}

TEST_F(OpenAiResponsesOrchestratorTest, ProcessResponseStoresAssistantText) {
  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-4o", "https://api.openai.com/v1");
  const std::string response = R"({
    "usage": { "input_tokens": 3, "output_tokens": 4 },
    "output": [
      {
        "type": "message",
        "content": [
          { "type": "output_text", "text": "Hello from responses" }
        ]
      }
    ]
  })";
  auto st_or = orchestrator.ProcessResponse("s1", response, "g1");
  ASSERT_TRUE(st_or.ok());
  EXPECT_EQ(*st_or, 7);

  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());
  ASSERT_EQ(history_or->size(), 1);
  EXPECT_EQ((*history_or)[0].role, "assistant");
  EXPECT_EQ((*history_or)[0].content, "Hello from responses");
}

TEST_F(OpenAiResponsesOrchestratorTest, ProcessResponseStoresFunctionCalls) {
  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-4o", "https://api.openai.com/v1");
  const std::string response = R"({
    "usage": { "input_tokens": 5, "output_tokens": 6 },
    "output": [
      {
        "type": "function_call",
        "call_id": "call_1",
        "name": "run_test",
        "arguments": "{\"cmd\":\"bazel test //...\"}"
      }
    ]
  })";
  auto st_or = orchestrator.ProcessResponse("s1", response, "g1");
  ASSERT_TRUE(st_or.ok());
  EXPECT_EQ(*st_or, 11);

  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());
  ASSERT_EQ(history_or->size(), 1);
  EXPECT_EQ((*history_or)[0].status, "tool_call");
  EXPECT_EQ((*history_or)[0].tool_call_id, "call_1|run_test");

  auto calls_or = orchestrator.ParseToolCalls((*history_or)[0]);
  ASSERT_TRUE(calls_or.ok());
  ASSERT_EQ(calls_or->size(), 1);
  EXPECT_EQ((*calls_or)[0].id, "call_1");
  EXPECT_EQ((*calls_or)[0].name, "run_test");
}

TEST_F(OpenAiResponsesOrchestratorTest, ProcessResponseSupportsObjectFunctionArguments) {
  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-4o", "https://api.openai.com/v1");
  const std::string response = R"({
    "usage": { "input_tokens": 5, "output_tokens": 6 },
    "output": [
      {
        "type": "function_call",
        "call_id": "call_obj",
        "name": "query_db",
        "arguments": { "sql": "SELECT 1 + 1" }
      }
    ]
  })";

  auto st_or = orchestrator.ProcessResponse("s1", response, "g1");
  ASSERT_TRUE(st_or.ok());

  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());
  ASSERT_EQ(history_or->size(), 1);

  auto calls_or = orchestrator.ParseToolCalls((*history_or)[0]);
  ASSERT_TRUE(calls_or.ok());
  ASSERT_EQ(calls_or->size(), 1);
  EXPECT_EQ((*calls_or)[0].name, "query_db");
  EXPECT_EQ(json_get_or((*calls_or)[0].args, "sql", std::string{}), "SELECT 1 + 1");
}

}  // namespace slop
