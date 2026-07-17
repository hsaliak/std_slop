#include "core/orchestrator_openai_responses.h"

#include <set>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

#include "core/database.h"
#include "core/http_client.h"
#include "core/json_utils.h"

#include <gtest/gtest.h>

namespace slop {

absl::StatusOr<nlohmann::json> BuildRequest(OpenAiResponsesOrchestrator& orchestrator, Database& db,
                                             const std::string& session_id, const std::string& system_instruction,
                                             const std::vector<Database::Message>& history,
                                             const std::vector<std::string>& active_skills) {
  (void)session_id;
  auto tools_or = db.GetTopLevelTools();
  if (!tools_or.ok()) return tools_or.status();
  std::string active_skill_content;
  if (!active_skills.empty()) {
    auto skills_or = db.GetSkills();
    if (!skills_or.ok()) return skills_or.status();
    active_skill_content = "## Active Personas & Skills\n";
    for (const auto& active_name : active_skills) {
      for (const auto& skill : *skills_or) {
        if (skill.name == active_name) {
          absl::StrAppend(&active_skill_content, "### Skill: ", skill.name, "\n", skill.system_prompt_patch, "\n");
          break;
        }
      }
    }
  }
  return orchestrator.BuildRequest(
      {system_instruction, history, std::move(*tools_or), std::move(active_skill_content)});
}

class OpenAiResponsesOrchestratorTest : public ::testing::Test {
 protected:
  Database db;
  HttpClient http;

  void SetUp() override { ASSERT_TRUE(db.Init(":memory:").ok()); }
};

TEST_F(OpenAiResponsesOrchestratorTest, BuildRequestDoesNotReadDatabase) {
  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-4o", "https://api.openai.com/v1");
  Database::Message user{0, "s1", "user", "Hello", "", "completed", "", "g1", "", 0};
  Database::Tool tool{"read_file", "Read a file", R"({"type":"object"})", true};

  auto payload_or = orchestrator.BuildRequest({"System prompt", {user}, {tool}, "Skill patch"});

  ASSERT_TRUE(payload_or.ok());
  EXPECT_EQ(json_get_or(*payload_or, "instructions", std::string{}), "System prompt");
  ASSERT_EQ((*payload_or)["input"].size(), 2);
  EXPECT_EQ(json_get_or((*payload_or)["input"][0], "role", std::string{}), "system");
  EXPECT_EQ(json_get_or((*payload_or)["input"][1], "role", std::string{}), "user");
  ASSERT_EQ((*payload_or)["tools"].size(), 1);
  EXPECT_EQ(json_get_or((*payload_or)["tools"][0], "name", std::string{}), "read_file");
}

TEST_F(OpenAiResponsesOrchestratorTest, AssemblePayloadBuildsInputAndTools) {
  ASSERT_TRUE(db.RegisterTool({"query_db", "Query database",
                               R"({"type":"object","properties":{"sql":{"type":"string"}},"required":["sql"]})", true})
                  .ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());

  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-4o", "https://api.openai.com/v1");
  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());
  auto payload_or = BuildRequest(orchestrator, db, "s1", "System prompt", *history_or, {});
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
  auto payload_or = BuildRequest(orchestrator, db, "s1", "Codex system instructions", *history_or, {});
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
  auto payload_or = BuildRequest(orchestrator, db, "s1", "Codex system instructions", *history_or, {});
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
  auto payload_or = BuildRequest(orchestrator, db, "s1", "Codex system instructions", *history_or, {});
  ASSERT_FALSE(payload_or.ok());
  EXPECT_EQ(payload_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(OpenAiResponsesOrchestratorTest, AssemblePayloadAppendsActiveSkillsToInputTail) {
  Database::Skill first = {0, "first", "desc1", "FIRST_PATCH"};
  Database::Skill second = {0, "second", "desc2", "SECOND_PATCH"};
  ASSERT_TRUE(db.RegisterSkill(first).ok());
  ASSERT_TRUE(db.RegisterSkill(second).ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());

  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-4o", "https://api.openai.com/v1");
  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());
  auto payload_or = BuildRequest(orchestrator, db, "s1", "System prompt", *history_or, {"second", "first"});
  ASSERT_TRUE(payload_or.ok());

  const auto& payload = *payload_or;
  std::string instructions = json_get_or(payload, "instructions", std::string{});
  EXPECT_FALSE(absl::StrContains(instructions, std::string("SECOND_PATCH")));
  EXPECT_FALSE(absl::StrContains(instructions, std::string("FIRST_PATCH")));

  ASSERT_TRUE(payload.contains("input"));
  ASSERT_TRUE(payload["input"].is_array());
  ASSERT_GE(payload["input"].size(), 2);

  // The skill system message should be the second-to-last item (before the user message).
  const auto& skill_item = payload["input"][payload["input"].size() - 2];
  EXPECT_EQ(json_get_or(skill_item, "role", std::string{}), "system");
  std::string skill_content = json_get_or(skill_item, "content", std::string{});
  EXPECT_LT(skill_content.find("SECOND_PATCH"), skill_content.find("FIRST_PATCH"));
}

TEST_F(OpenAiResponsesOrchestratorTest, AssemblePayloadOmitsSkillSectionWhenNoActiveSkills) {
  Database::Skill skill = {0, "test_skill", "desc", "PATCH"};
  ASSERT_TRUE(db.RegisterSkill(skill).ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());

  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-4o", "https://api.openai.com/v1");
  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());
  auto payload_or = BuildRequest(orchestrator, db, "s1", "System prompt", *history_or, {});
  ASSERT_TRUE(payload_or.ok());

  const auto& payload = *payload_or;
  ASSERT_TRUE(payload.contains("input"));
  ASSERT_TRUE(payload["input"].is_array());
  for (const auto& item : payload["input"]) {
    EXPECT_NE(json_get_or(item, "role", std::string{}), "system");
  }
}

TEST_F(OpenAiResponsesOrchestratorTest, AssemblePayloadCacheKeyStableAcrossSkillChanges) {
  Database::Skill first = {0, "first", "desc1", "FIRST_PATCH"};
  Database::Skill second = {0, "second", "desc2", "SECOND_PATCH"};
  ASSERT_TRUE(db.RegisterSkill(first).ok());
  ASSERT_TRUE(db.RegisterSkill(second).ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());

  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-4o", "https://api.openai.com/v1");
  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());

  auto with_first = BuildRequest(orchestrator, db, "s1", "System prompt", *history_or, {"first"});
  ASSERT_TRUE(with_first.ok());
  ASSERT_TRUE(with_first->contains("prompt_cache_key"));
  std::string key_with_first = json_get_or(*with_first, "prompt_cache_key", std::string{});
  EXPECT_TRUE(absl::StartsWith(key_with_first, "slop:"));
  EXPECT_EQ(key_with_first.size(), 64);  // Provider limit: "slop:" (5) + 59 hex chars.

  auto with_second = BuildRequest(orchestrator, db, "s1", "System prompt", *history_or, {"second"});
  ASSERT_TRUE(with_second.ok());
  std::string key_with_second = json_get_or(*with_second, "prompt_cache_key", std::string{});

  // Same static instructions -> same cache key, even though active skills differ.
  EXPECT_EQ(key_with_first, key_with_second);
}

TEST_F(OpenAiResponsesOrchestratorTest, AssemblePayloadCacheKeyChangesWithDifferentInstructions) {
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());

  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-4o", "https://api.openai.com/v1");
  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());

  auto payload_a = BuildRequest(orchestrator, db, "s1", "Instructions A", *history_or, {});
  ASSERT_TRUE(payload_a.ok());
  ASSERT_TRUE(payload_a->contains("prompt_cache_key"));
  std::string key_a = json_get_or(*payload_a, "prompt_cache_key", std::string{});

  auto payload_b = BuildRequest(orchestrator, db, "s1", "Instructions B", *history_or, {});
  ASSERT_TRUE(payload_b.ok());
  std::string key_b = json_get_or(*payload_b, "prompt_cache_key", std::string{});

  EXPECT_NE(key_a, key_b);
}

TEST_F(OpenAiResponsesOrchestratorTest, AssemblePayloadOmitsCacheKeyForEmptyInstructions) {
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());

  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-4o", "https://api.openai.com/v1");
  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());
  auto payload_or = BuildRequest(orchestrator, db, "s1", "", *history_or, {});
  ASSERT_TRUE(payload_or.ok());
  EXPECT_FALSE(payload_or->contains("prompt_cache_key"));
}

TEST_F(OpenAiResponsesOrchestratorTest, AssemblePayloadCacheKeyIdempotentAfterDbRoundTrip) {
  // Verify that hashing the same system instruction string produces the same cache key,
  // simulating a write-to-DB and re-read cycle where content is identical.
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());

  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-4o", "https://api.openai.com/v1");
  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());

  const std::string instruction = "Idempotent test instruction";

  // First payload — cache key derived from instruction.
  auto first = BuildRequest(orchestrator, db, "s1", instruction, *history_or, {});
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(first->contains("prompt_cache_key"));
  std::string key_first = json_get_or(*first, "prompt_cache_key", std::string{});

  // Simulate round-trip: store instruction in DB, re-read, re-hash.
  ASSERT_TRUE(db.SetAgentMd("./AGENTS.md", instruction).ok());
  auto reloaded = db.GetAgentMd("./AGENTS.md");
  ASSERT_TRUE(reloaded.ok());
  ASSERT_EQ(*reloaded, instruction);

  auto second = BuildRequest(orchestrator, db,"s1", *reloaded, *history_or, {});
  ASSERT_TRUE(second.ok());
  std::string key_second = json_get_or(*second, "prompt_cache_key", std::string{});

  EXPECT_EQ(key_first, key_second);
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

TEST_F(OpenAiResponsesOrchestratorTest, ExtractAssistantTextParsesSsePayload) {
  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-oss-120b", "https://chatgpt.com/backend-api/codex");
  const std::string sse_payload =
      "event: response.output_text.delta\n"
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Compact\"}\n\n"
      "event: response.output_text.delta\n"
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\" summary\"}\n\n"
      "event: response.completed\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp1\","
      "\"usage\":{\"input_tokens\":2,\"output_tokens\":3}}}\n\n";

  auto text_or = orchestrator.ExtractAssistantText(sse_payload);
  ASSERT_TRUE(text_or.ok()) << text_or.status();
  EXPECT_EQ(*text_or, "Compact summary");
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

TEST_F(OpenAiResponsesOrchestratorTest, PreservesCompletedOutputItemsInMemory) {
  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-oss-120b", "https://api.openai.com/v1");
  const nlohmann::json response = {
      {"output",
       {{{"id", "reason_1"},
         {"type", "reasoning"},
         {"status", "completed"},
         {"summary", {{{"type", "summary_text"}, {"text", "planned"}}}},
         {"encrypted_content", "opaque"}},
        {{"id", "msg_1"},
         {"type", "message"},
         {"status", "completed"},
         {"role", "assistant"},
         {"content", {{{"type", "output_text"}, {"text", "Done"}}}}}}}};

  ASSERT_TRUE(orchestrator.ProcessResponse("s1", response.dump(), "g1").ok());
  const auto& items = orchestrator.GetLastOutputItems();
  ASSERT_EQ(items.size(), 2);
  EXPECT_EQ(items[0].id, "reason_1");
  EXPECT_EQ(items[0].type, "reasoning");
  EXPECT_EQ(items[0].status, "completed");
  EXPECT_EQ(json_get_or(items[0].raw, "encrypted_content", std::string{}), "opaque");
  EXPECT_EQ(items[1].id, "msg_1");
}

TEST_F(OpenAiResponsesOrchestratorTest, ParsesToolCallsFromActiveOutputItems) {
  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-oss-120b", "https://api.openai.com/v1");
  const nlohmann::json response = {
      {"output", nlohmann::json::array({{{"id", "fc_1"}, {"type", "function_call"},
                                           {"call_id", "call_1"}, {"name", "query_db"},
                                           {"arguments", "{\"sql\":\"select 1\"}"}}})}};

  ASSERT_TRUE(orchestrator.ProcessResponse("s1", response.dump(), "g1").ok());
  auto calls_or = orchestrator.ParseLastOutputToolCalls();
  ASSERT_TRUE(calls_or.ok());
  ASSERT_EQ(calls_or->size(), 1);
  EXPECT_EQ((*calls_or)[0].id, "call_1");
  EXPECT_EQ((*calls_or)[0].name, "query_db");
  EXPECT_EQ((*calls_or)[0].args["sql"], "select 1");
}

TEST_F(OpenAiResponsesOrchestratorTest, ClearsUsageWhenResponseOmitsUsage) {
  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-oss-120b", "https://api.openai.com/v1");
  const nlohmann::json with_usage = {
      {"usage", {{"input_tokens", 10}, {"output_tokens", 2}, {"input_tokens_details", {{"cached_tokens", 8}}}}},
      {"output", {{{"type", "message"},
                   {"role", "assistant"},
                   {"content", {{{"type", "output_text"}, {"text", "First"}}}}}}}};
  ASSERT_TRUE(orchestrator.ProcessResponse("s1", with_usage.dump(), "g1").ok());
  ASSERT_TRUE(orchestrator.GetLastResponseUsage().has_value());

  const nlohmann::json without_usage = {
      {"output", {{{"type", "message"},
                   {"role", "assistant"},
                   {"content", {{{"type", "output_text"}, {"text", "Second"}}}}}}}};
  ASSERT_TRUE(orchestrator.ProcessResponse("s1", without_usage.dump(), "g2").ok());
  EXPECT_FALSE(orchestrator.GetLastResponseUsage().has_value());
}

TEST_F(OpenAiResponsesOrchestratorTest, ExtractAssistantTextParsesCompletedResponseOutput) {
  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-oss-120b", "https://chatgpt.com/backend-api/codex");
  const std::string sse_payload =
      "event: response.created\n"
      "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp1\",\"output\":[]}}\n\n"
      "event: response.completed\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp1\","
      "\"usage\":{\"input_tokens\":4,\"output_tokens\":5},\"output\":[{"
      "\"type\":\"message\",\"role\":\"assistant\",\"content\":[{"
      "\"type\":\"output_text\",\"text\":\"Completed compact summary\"}]}]}}\n\n";

  auto text_or = orchestrator.ExtractAssistantText(sse_payload);
  ASSERT_TRUE(text_or.ok()) << text_or.status();
  EXPECT_EQ(*text_or, "Completed compact summary");
}

TEST_F(OpenAiResponsesOrchestratorTest, ExtractAssistantTextDoesNotDuplicateCompletedResponseOutput) {
  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-oss-120b", "https://chatgpt.com/backend-api/codex");
  const std::string sse_payload =
      "event: response.output_item.done\n"
      "data: {\"type\":\"response.output_item.done\",\"item\":{"
      "\"type\":\"message\",\"role\":\"assistant\",\"content\":[{"
      "\"type\":\"output_text\",\"text\":\"Already streamed\"}]}}\n\n"
      "event: response.completed\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp1\","
      "\"usage\":{\"input_tokens\":4,\"output_tokens\":5},\"output\":[{"
      "\"type\":\"message\",\"role\":\"assistant\",\"content\":[{"
      "\"type\":\"output_text\",\"text\":\"Already streamed\"}]}]}}\n\n";

  auto text_or = orchestrator.ExtractAssistantText(sse_payload);
  ASSERT_TRUE(text_or.ok()) << text_or.status();
  EXPECT_EQ(*text_or, "Already streamed");
}

TEST_F(OpenAiResponsesOrchestratorTest, ProcessResponseParsesSseOutputTextDoneFallback) {
  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-oss-120b", "https://chatgpt.com/backend-api/codex");
  const std::string sse_payload =
      "event: response.created\n"
      "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp1\",\"output\":[]}}\n\n"
      "event: response.output_text.done\n"
      "data: {\"type\":\"response.output_text.done\",\"text\":\"Done compact summary\"}\n\n"
      "event: response.completed\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp1\","
      "\"usage\":{\"input_tokens\":6,\"output_tokens\":7}}}\n\n";

  auto st_or = orchestrator.ProcessResponse("s1", sse_payload, "g1");
  ASSERT_TRUE(st_or.ok()) << st_or.status();
  EXPECT_EQ(*st_or, 13);

  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());
  ASSERT_EQ(history_or->size(), 1);
  EXPECT_EQ((*history_or)[0].role, "assistant");
  EXPECT_EQ((*history_or)[0].content, "Done compact summary");
}

TEST_F(OpenAiResponsesOrchestratorTest, ProcessResponseUsesOutputTextDoneAsFinalText) {
  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-oss-120b", "https://chatgpt.com/backend-api/codex");
  const std::string sse_payload =
      "event: response.output_text.delta\n"
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Part\"}\n\n"
      "event: response.output_text.delta\n"
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"ial\"}\n\n"
      "event: response.output_text.done\n"
      "data: {\"type\":\"response.output_text.done\",\"text\":\"Final compact summary\"}\n\n"
      "event: response.completed\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp1\","
      "\"usage\":{\"input_tokens\":6,\"output_tokens\":7}}}\n\n";

  auto st_or = orchestrator.ProcessResponse("s1", sse_payload, "g1");
  ASSERT_TRUE(st_or.ok()) << st_or.status();

  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());
  ASSERT_EQ(history_or->size(), 1);
  EXPECT_EQ((*history_or)[0].content, "Final compact summary");
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
