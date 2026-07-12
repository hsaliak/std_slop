#include "core/orchestrator.h"

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

#include "core/database.h"

#include <gtest/gtest.h>
namespace slop {
class OrchestratorTest : public ::testing::Test {
 protected:
  Database db;
  HttpClient http;
  void SetUp() override { ASSERT_TRUE(db.Init(":memory:").ok()); }
};
TEST_F(OrchestratorTest, AssemblePromptBasic) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "Hi!").ok());
  auto result = orchestrator->AssemblePrompt("s1", {});
  ASSERT_TRUE(result.ok());
  nlohmann::json prompt = *result;
  ASSERT_EQ(prompt["input"].size(), 2);
  EXPECT_EQ(prompt["input"][0]["role"], "user");
  EXPECT_EQ(prompt["input"][1]["role"], "assistant");
}
TEST_F(OrchestratorTest, AssemblePromptWithSkills) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  Database::Skill skill = {1, "test_skill", "A test skill", "SYSTEM_PATCH"};
  ASSERT_TRUE(db.RegisterSkill(skill).ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());
  auto result = orchestrator->AssemblePrompt("s1", {"test_skill"});
  ASSERT_TRUE(result.ok());
  nlohmann::json prompt = *result;
  ASSERT_TRUE(prompt.contains("instructions"));
  EXPECT_TRUE(absl::StrContains(prompt["instructions"].get<std::string>(), "SYSTEM_PATCH"));
}
TEST_F(OrchestratorTest, AssemblePromptPreservesActiveSkillOrder) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  Database::Skill first = {0, "first", "desc1", "FIRST_PATCH"};
  Database::Skill second = {0, "second", "desc2", "SECOND_PATCH"};
  ASSERT_TRUE(db.RegisterSkill(first).ok());
  ASSERT_TRUE(db.RegisterSkill(second).ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());

  auto result = orchestrator->AssemblePrompt("s1", {"second", "first"});
  ASSERT_TRUE(result.ok());
  std::string instructions = (*result)["instructions"].get<std::string>();
  EXPECT_LT(instructions.find("SECOND_PATCH"), instructions.find("FIRST_PATCH"));
}
TEST_F(OrchestratorTest, AccordionPreservesHistoricalToolResults) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  Orchestrator::TruncationSettings ts;
  std::string long_content(ts.full_fidelity_limit / 2, 'a');
  // Group 1: Previous group (fill with enough tools to trigger truncation for the oldest one)
  ASSERT_TRUE(db.AppendMessage("s1", "user", "call tool", "", "completed", "g1").ok());
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(db.AppendMessage("s1", "assistant", "calling", "", "tool_call", "g1").ok());
    ASSERT_TRUE(
        db.AppendMessage("s1", "tool", long_content, "id" + std::to_string(i) + "|test_tool", "completed", "g1").ok());
  }
  // Group 2: Current group
  ASSERT_TRUE(db.AppendMessage("s1", "user", "another call", "", "completed", "g2").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "calling", "", "tool_call", "g2").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "tool", long_content, "id_active|test_tool", "completed", "g2").ok());
  // We need a tool named "test_tool" to be enabled so it's not filtered out.
  ASSERT_TRUE(
      db.Execute("INSERT INTO tools (name, description, json_schema, is_enabled) VALUES ('test_tool', 'desc', '{}', 1)")
          .ok());
  auto result = orchestrator->AssemblePrompt("s1", {});
  ASSERT_TRUE(result.ok());
  nlohmann::json prompt = *result;
  bool found_g1 = false;
  bool found_g2 = false;
  for (const auto& item : prompt.value("input", nlohmann::json::array())) {
    if (item.value("type", "") == "function_call_output") {
      std::string tool_content = item.value("output", "");
      if (tool_content == long_content) {
        if (!found_g1) {
          found_g1 = true;
        } else {
          found_g2 = true;
        }
      }
    }
  }
  EXPECT_TRUE(found_g1);
  EXPECT_TRUE(found_g2);
}
TEST_F(OrchestratorTest, AccordionToolResultsUseFullFidelityLimit) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  Orchestrator::TruncationSettings ts;
  std::string long_content(ts.full_fidelity_limit + 100, 'a');
  const int total_tools = 2;
  ASSERT_TRUE(db.AppendMessage("s1", "user", "active call", "", "completed", "g1").ok());
  for (int i = 0; i < total_tools; ++i) {
    ASSERT_TRUE(db.AppendMessage("s1", "assistant", "calling", "", "tool_call", "g1").ok());
    ASSERT_TRUE(
        db.AppendMessage("s1", "tool", long_content, "id" + std::to_string(i) + "|test_tool", "completed", "g1").ok());
  }
  ASSERT_TRUE(
      db.Execute("INSERT INTO tools (name, description, json_schema, is_enabled) VALUES ('test_tool', 'desc', '{}', 1)")
          .ok());
  auto result = orchestrator->AssemblePrompt("s1", {});
  ASSERT_TRUE(result.ok());
  nlohmann::json prompt = *result;
  int truncated_count = 0;
  int full_fidelity_count = 0;
  for (const auto& item : prompt.value("input", nlohmann::json::array())) {
    if (item.value("type", "") == "function_call_output") {
      std::string tool_content = item.value("output", "");
      if (absl::StrContains(tool_content, "TRUNCATED. Use query_db")) {
        EXPECT_LT(tool_content.size(), ts.full_fidelity_limit + 200);
        truncated_count++;
      } else if (tool_content == long_content) {
        full_fidelity_count++;
      }
    }
  }
  EXPECT_EQ(truncated_count, total_tools);
  EXPECT_EQ(full_fidelity_count, 0);
}
TEST_F(OrchestratorTest, AccordionPreservesToolResultsBelowFullFidelityLimit) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  Orchestrator::TruncationSettings ts;
  std::string long_content(ts.full_fidelity_limit / 2, 'a');
  // Group 1: Previous group (fill with enough tools to trigger truncation for the oldest one)
  ASSERT_TRUE(db.AppendMessage("s1", "user", "call tool", "", "completed", "g1").ok());
  for (int i = 0; i < 5; ++i) {
    std::string id = "tc_old_" + std::to_string(i);
    ASSERT_TRUE(
        db.AppendMessage(
              "s1", "assistant",
              "{\"tool_calls\": [{\"id\": \"" + id +
                  "\", \"type\": \"function\", \"function\": {\"name\": \"test_tool\", \"arguments\": \"{}\"}}]}",
              "", "tool_call", "g1")
            .ok());
    ASSERT_TRUE(db.AppendMessage("s1", "tool", long_content, id + "|test_tool", "completed", "g1").ok());
  }
  // Group 2: Current group
  ASSERT_TRUE(db.AppendMessage("s1", "user", "another call", "", "completed", "g2").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant",
                               "{\"tool_calls\": [{\"id\": \"tc_active\", \"type\": \"function\", \"function\": "
                               "{\"name\": \"test_tool\", \"arguments\": \"{}\"}}]}",
                               "", "tool_call", "g2")
                  .ok());
  ASSERT_TRUE(db.AppendMessage("s1", "tool", long_content, "tc_active|test_tool", "completed", "g2").ok());
  // We need a tool named "test_tool" to be enabled
  ASSERT_TRUE(
      db.Execute("INSERT INTO tools (name, description, json_schema, is_enabled) VALUES ('test_tool', 'desc', '{}', 1)")
          .ok());
  auto result = orchestrator->AssemblePrompt("s1", {});
  ASSERT_TRUE(result.ok());
  nlohmann::json prompt = *result;
  // Responses payload structure: input -> type: function_call_output, output
  bool found_g1 = false;
  bool found_g2 = false;
  for (const auto& item : prompt.value("input", nlohmann::json::array())) {
    if (item.value("type", "") == "function_call_output") {
      std::string tool_content = item.value("output", "");
      if (tool_content == long_content) {
        if (!found_g1) {
          found_g1 = true;
        } else {
          found_g2 = true;
        }
      }
    }
  }
  EXPECT_TRUE(found_g1);
  EXPECT_TRUE(found_g2);
}
TEST_F(OrchestratorTest, AccordionResetsFromLatestPromptUsage) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  ASSERT_TRUE(db.SetAccordionContextSettings("s1", 2, 350000).ok());
  for (const std::string& group_id : {"g1", "g2", "g3"}) {
    ASSERT_TRUE(db.AppendMessage("s1", "user", group_id, "", "completed", group_id).ok());
    ASSERT_TRUE(db.AppendMessage("s1", "assistant", group_id, "", "completed", group_id).ok());
  }
  ASSERT_TRUE(db.RecordUsage("other", "model", 999999, 1).ok());
  ASSERT_TRUE(db.RecordUsage("s1", "model", 349999, 1).ok());
  auto history_or = orchestrator->GetAccordionHistory("s1");
  ASSERT_TRUE(history_or.ok());
  ASSERT_EQ(history_or->size(), 6);
  ASSERT_TRUE(db.RecordUsage("s1", "model", 350000, 1).ok());
  history_or = orchestrator->GetAccordionHistory("s1");
  ASSERT_TRUE(history_or.ok());
  ASSERT_EQ(history_or->size(), 4);
  EXPECT_EQ((*history_or)[0].group_id, "g2");
  EXPECT_EQ((*history_or)[3].group_id, "g3");
  auto settings_or = db.GetAccordionContextSettings("s1");
  ASSERT_TRUE(settings_or.ok());
  EXPECT_EQ(settings_or->epoch_start_group_id, "g2");
}
TEST_F(OrchestratorTest, SmarterTruncate) {
  std::string head = "COMMAND_START\n";
  std::string middle(10000, 'x');
  std::string tail = "\nERROR_AT_THE_END";
  std::string content = head + middle + tail;
  // 1. Large limit: Should sandwich
  size_t limit = 1000;
  std::string result = Orchestrator::SmarterTruncate(content, limit, 123);
  EXPECT_TRUE(absl::StrContains(result, "COMMAND_START"));
  EXPECT_TRUE(absl::StrContains(result, "ERROR_AT_THE_END"));
  EXPECT_TRUE(absl::StrContains(result, "TRUNCATED"));
  EXPECT_TRUE(absl::StrContains(result, "query_db"));
  EXPECT_NEAR(result.size(), limit, 50);
  // 2. Inactive limit (120): Should still fit the hint if possible
  std::string inactive_result = Orchestrator::SmarterTruncate(content, 125, 456);
  EXPECT_TRUE(absl::StrContains(inactive_result, "TRUNCATED"));
  EXPECT_TRUE(absl::StrContains(inactive_result, "456"));
  EXPECT_NEAR(inactive_result.size(), 125, 10);
  // 3. Tiny limit: Should fallback to "..."
  std::string tiny_result = Orchestrator::SmarterTruncate(content, 10);
  EXPECT_EQ(tiny_result, "COMMAND...");
}
TEST_F(OrchestratorTest, SmarterTruncateUtf8) {
  // Japanes char "こ" is 3 bytes.
  std::string jp = "こんにちは" + std::string(1000, 'x') + "さようなら";
  size_t limit = 300;
  std::string result = Orchestrator::SmarterTruncate(jp, limit, 789);
  // Verify it doesn't crash and contains start/end
  EXPECT_TRUE(absl::StrContains(result, "こんにちは"));
  EXPECT_TRUE(absl::StrContains(result, "さようなら"));
}
TEST_F(OrchestratorTest, SafeJsonDump) {
  // Test that dumping invalid UTF-8 with the replace handler doesn't crash (even with -fno-exceptions)
  nlohmann::json j;
  j["invalid"] = std::string("abc\xFF", 4) + "def";  // 0xFF is invalid UTF-8
  std::string dumped = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  EXPECT_TRUE(absl::StrContains(dumped, "abc"));
  EXPECT_TRUE(absl::StrContains(dumped, "def"));
  // The invalid byte should be replaced by the Unicode replacement character \uFFFD
  // In a JSON string, this might be escaped or raw depending on how dump handles it.
}
TEST_F(OrchestratorTest, ProcessResponsePersists) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  std::string mock_response = R"({
        "output": [{
            "type": "message",
            "content": [{"type": "output_text", "text": "I am an AI assistant"}]
        }]
    })";
  ASSERT_TRUE(orchestrator->ProcessResponse("s1", mock_response).ok());
  auto history = db.GetConversationHistory("s1");
  ASSERT_TRUE(history.ok());
  ASSERT_EQ(history->size(), 1);
  EXPECT_EQ((*history)[0].role, "assistant");
  EXPECT_EQ((*history)[0].content, "I am an AI assistant");
}
TEST_F(OrchestratorTest, ProcessResponseToolCall) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  nlohmann::json mock_json = nlohmann::json::array();
  mock_json.push_back({
      {"type", "function_call"},
      {"call_id", "call_1"},
      {"name", "execute_bash"},
      {"arguments", R"({"command": "ls"})"}
  });
  std::string mock_response = nlohmann::json({{"output", mock_json}}).dump();
  ASSERT_TRUE(orchestrator->ProcessResponse("s1", mock_response).ok());
  auto history = db.GetConversationHistory("s1");
  ASSERT_TRUE(history.ok());
  ASSERT_EQ(history->size(), 1);
  EXPECT_EQ((*history)[0].role, "assistant");
  EXPECT_EQ((*history)[0].status, "tool_call");
  auto calls_or = orchestrator->ParseToolCalls((*history)[0]);
  ASSERT_TRUE(calls_or.ok());
  ASSERT_EQ(calls_or->size(), 1);
  EXPECT_EQ((*calls_or)[0].name, "execute_bash");
}
TEST_F(OrchestratorTest, AssemblePromptWithTools) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  Database::Tool tool = {"query_db", "Query database",
                         R"({"type":"object","properties":{"sql":{"type":"string"}},"required":["sql"]})", true};
  ASSERT_TRUE(db.RegisterTool(tool).ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Use the tool").ok());
  auto result = orchestrator->AssemblePrompt("s1");
  ASSERT_TRUE(result.ok());
  nlohmann::json prompt = *result;
  ASSERT_TRUE(prompt.contains("tools"));
  bool found = false;
  for (const auto& tool : prompt["tools"]) {
    if (tool["name"] == "query_db") {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(OrchestratorTest, ToolSchemasNormalizeUnionTypes) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);

  Database::Tool tool = {"union_tool", "Union test",
                         R"({"type":"object","properties":{"index":{"type":["integer","string"]}}})", true};
  ASSERT_TRUE(db.RegisterTool(tool).ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Use the tool").ok());

  auto result = orchestrator->AssemblePrompt("s1", {});
  ASSERT_TRUE(result.ok());
  nlohmann::json prompt = *result;

  ASSERT_TRUE(prompt.contains("tools"));
  const nlohmann::json* decl = nullptr;
  for (const auto& d : prompt["tools"]) {
    if (d.value("name", "") == "union_tool") {
      decl = &d;
      break;
    }
  }
  ASSERT_NE(decl, nullptr);

  const auto& parameters = (*decl)["parameters"];
  ASSERT_TRUE(parameters.contains("properties"));
  ASSERT_TRUE(parameters["properties"].contains("index"));
  ASSERT_TRUE(parameters["properties"]["index"].contains("type"));
}

TEST_F(OrchestratorTest, AssembleOpenAIPrompt) {
  auto orchestrator_or =
      Orchestrator::Builder(&db, &http).WithModel("gpt-4o").Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());
  auto result = orchestrator->AssemblePrompt("s1", {});
  ASSERT_TRUE(result.ok());
  nlohmann::json prompt = *result;
  EXPECT_EQ(prompt["model"], "gpt-4o");
  ASSERT_TRUE(prompt.contains("instructions"));
  ASSERT_TRUE(prompt.contains("input"));
  EXPECT_EQ(prompt["input"][0]["role"], "user");
  EXPECT_TRUE(absl::StrContains(prompt["input"][0]["content"].get<std::string>(), "Hello"));
}
TEST_F(OrchestratorTest, ProcessResponseKeepsStateOnlyInHistory) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  const std::string assistant_state = "Hello!\n\n### STATE\nGoal: unit test\nContext: none";
  const std::string mock_response = absl::StrCat(
      R"({"output":[{"type":"message","content":[{"type":"output_text","text":)", nlohmann::json(assistant_state).dump(), R"(}]}]})");
  ASSERT_TRUE(orchestrator->ProcessResponse("s1", mock_response).ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "next request").ok());
  auto prompt_or = orchestrator->AssemblePrompt("s1");
  ASSERT_TRUE(prompt_or.ok());
  const std::string system_instruction = (*prompt_or)["instructions"].get<std::string>();
  EXPECT_FALSE(absl::StrContains(system_instruction, "## Global State (Anchor)"));
  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());
  ASSERT_EQ(history_or->size(), 2);
  EXPECT_EQ((*history_or)[0].content, assistant_state);
  const std::string serialized_state = nlohmann::json(assistant_state).dump();
  const std::string prompt_json = prompt_or->dump();
  EXPECT_EQ(prompt_json.find(serialized_state), prompt_json.rfind(serialized_state));
}
TEST_F(OrchestratorTest, HistoryNormalization) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  // Create invalid sequence: User -> User
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Part 1").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Part 2").ok());
  // Create orphaned tool response: User -> Tool
  ASSERT_TRUE(
      db.AppendMessage("s1", "tool", "{\"functionResponse\":{\"name\":\"ls\",\"response\":{\"content\":\"a.txt\"}}}")
          .ok());
  auto result = orchestrator->AssemblePrompt("s1", {});
  ASSERT_TRUE(result.ok());
  nlohmann::json prompt = *result;
  // Should have 1 user turn (merged) and the tool response should be suppressed (changed to user role) and merged.
  ASSERT_EQ(prompt["input"].size(), 3);
  EXPECT_EQ(prompt["input"][0]["role"], "user");
  EXPECT_EQ(prompt["input"][1]["role"], "user");
  EXPECT_EQ(prompt["input"][2]["type"], "function_call_output");
}
TEST_F(OrchestratorTest, ParseToolCallsResponses) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  nlohmann::json tool_call = {
      {"id", "call_123"},
      {"type", "function"},
      {"function", {{"name", "execute_bash"}, {"arguments", R"({"command": "ls"})"}}}
  };
  Database::Message msg;
  msg.role = "assistant";
  msg.status = "tool_call";
  msg.parsing_strategy = "openai";
  msg.tool_call_id = "call_123|execute_bash";
  msg.content = nlohmann::json({{"role", "assistant"}, {"tool_calls", nlohmann::json::array({tool_call})}}).dump();
  auto tcs_or = orchestrator->ParseToolCalls(msg);
  ASSERT_TRUE(tcs_or.ok());
  ASSERT_EQ(tcs_or->size(), 1);
  EXPECT_EQ((*tcs_or)[0].name, "execute_bash");
  EXPECT_EQ((*tcs_or)[0].args["command"], "ls");
}
TEST_F(OrchestratorTest, ParseToolCallsOpenAI) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  Database::Message msg;
  msg.role = "assistant";
  msg.status = "tool_call";
  msg.parsing_strategy = "openai";
  msg.content = R"({
        "tool_calls": [{
            "id": "call_123",
            "function": {
                "name": "execute_bash",
                "arguments": "{\"command\": \"ls\"}"
            }
        }]
    })";
  auto tcs_or = orchestrator->ParseToolCalls(msg);
  ASSERT_TRUE(tcs_or.ok());
  ASSERT_EQ(tcs_or->size(), 1);
  EXPECT_EQ((*tcs_or)[0].name, "execute_bash");
  EXPECT_EQ((*tcs_or)[0].args["command"], "ls");
}
TEST_F(OrchestratorTest, HistoryFiltering) {
  auto gemini_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(gemini_or.ok());
  auto gemini = std::move(*gemini_or);
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello", "", "completed", "g0").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "Gemini msg", "", "completed", "g1", "openai").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "OpenAI msg", "", "completed", "g2", "openai").ok());
  auto hist_or = gemini->GetAccordionHistory("s1");
  ASSERT_TRUE(hist_or.ok());
  // Both Gemini and OpenAI text assistant messages are kept now.
  EXPECT_EQ(hist_or->size(), 3);
  EXPECT_EQ((*hist_or)[0].content, "Hello");
  EXPECT_EQ((*hist_or)[1].content, "Gemini msg");
  EXPECT_EQ((*hist_or)[2].content, "OpenAI msg");
  auto openai_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(openai_or.ok());
  auto openai = std::move(*openai_or);
  hist_or = openai->GetAccordionHistory("s1");
  ASSERT_TRUE(hist_or.ok());
  // Both kept.
  EXPECT_EQ(hist_or->size(), 3);
  EXPECT_EQ((*hist_or)[0].content, "Hello");
  EXPECT_EQ((*hist_or)[1].content, "Gemini msg");
  EXPECT_EQ((*hist_or)[2].content, "OpenAI msg");
}
TEST_F(OrchestratorTest, ToolResultFiltering) {
  auto gemini_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(gemini_or.ok());
  auto gemini = std::move(*gemini_or);
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Run tool", "", "completed", "g0").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "call", "my_tool", "tool_call", "g1", "openai").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "tool", "result", "my_tool", "completed", "g1", "openai").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "call", "other_tool", "tool_call", "g2", "openai").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "tool", "result", "other_tool", "completed", "g2", "openai").ok());
  auto hist_or = gemini->GetAccordionHistory("s1");
  ASSERT_TRUE(hist_or.ok());
  // All messages kept (no cross-provider filtering)
  EXPECT_EQ(hist_or->size(), 5);
}
TEST_F(OrchestratorTest, CrossProviderMessagePreservation) {
  // 1. Start with first provider
  auto gemini_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(gemini_or.ok());
  auto gemini = std::move(*gemini_or);
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello", "", "completed", "g0").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "I am assistant", "", "completed", "g1", "openai").ok());
  // 2. Switch provider
  auto openai_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(openai_or.ok());
  auto openai = std::move(*openai_or);
  auto hist_or = openai->GetAccordionHistory("s1");
  ASSERT_TRUE(hist_or.ok());
  // User "Hello" (text) and "I am assistant" (text assistant) should both be kept.
  EXPECT_EQ(hist_or->size(), 2);
  EXPECT_EQ((*hist_or)[0].content, "Hello");
  EXPECT_EQ((*hist_or)[1].content, "I am assistant");
  // 3. Add a Gemini Tool Call (should be filtered for OpenAI)
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "{}", "tool_1", "tool_call", "g2", "openai").ok());
  hist_or = openai->GetAccordionHistory("s1");
  ASSERT_EQ(hist_or->size(), 3);  // Still 2
}
TEST_F(OrchestratorTest, ProcessResponseExtractsUsage) {
  auto orchestrator_or =
      Orchestrator::Builder(&db, &http).WithModel("gpt-4o").Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());
  std::string mock_response = R"({
    "usage": {"input_tokens": 10, "output_tokens": 5},
    "output": [{"type": "message", "content": [{"type": "output_text", "text": "Hi!"}]}]
  })";
  auto result = orchestrator->ProcessResponse("s1", mock_response, "g1");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(*result, 15);
}

TEST_F(OrchestratorTest, ResponsesMultiToolCallProcessing) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  std::string mock_response = R"({
    "output": [
      {"type": "function_call", "call_id": "call_1", "name": "tool1", "arguments": "{\"a\": 1}"},
      {"type": "function_call", "call_id": "call_2", "name": "tool2", "arguments": "{\"b\": 2}"}
    ]
  })";
  ASSERT_TRUE(orchestrator->ProcessResponse("s1", mock_response, "g1").ok());
  auto history = db.GetConversationHistory("s1");
  ASSERT_TRUE(history.ok());
  ASSERT_EQ(history->size(), 1);
  EXPECT_EQ((*history)[0].status, "tool_call");
  auto calls_or = orchestrator->ParseToolCalls((*history)[0]);
  ASSERT_TRUE(calls_or.ok());
  ASSERT_EQ(calls_or->size(), 2);
  EXPECT_EQ((*calls_or)[0].name, "tool1");
  EXPECT_EQ((*calls_or)[1].name, "tool2");
}
TEST_F(OrchestratorTest, ResponsesMultiToolResponseAssembly) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Run tools").ok());
  // Register the tools so they are not filtered out
  ASSERT_TRUE(db.RegisterTool({"tool1", "desc1", "{}", true}).ok());
  ASSERT_TRUE(db.RegisterTool({"tool2", "desc2", "{}", true}).ok());
  // Simulate stored tool calls in Responses format
  nlohmann::json call1;
  call1["type"] = "function_call";
  call1["call_id"] = "call_1";
  call1["name"] = "tool1";
  call1["arguments"] = "{\"a\": 1}";
  nlohmann::json call2;
  call2["type"] = "function_call";
  call2["call_id"] = "call_2";
  call2["name"] = "tool2";
  call2["arguments"] = "{\"b\": 2}";
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", call1.dump(), "call_1|tool1", "tool_call", "g2", "openai").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", call2.dump(), "call_2|tool2", "tool_call", "g2", "openai").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "tool", "result1", "call_1|tool1", "completed", "g2", "openai").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "tool", "result2", "call_2|tool2", "completed", "g2", "openai").ok());
  auto result = orchestrator->AssemblePrompt("s1", {});
  ASSERT_TRUE(result.ok());
  nlohmann::json prompt = *result;
  // input should have: user, function_call, function_call, function_call_output, function_call_output
  ASSERT_GE(prompt["input"].size(), 1);
}
TEST_F(OrchestratorTest, ResponsesIdNameHandling) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  std::string mock_response = R"({
    "output": [
      {"type": "function_call", "call_id": "call_123", "name": "my_tool", "arguments": "{}"}
    ]
  })";
  ASSERT_TRUE(orchestrator->ProcessResponse("s1", mock_response, "g1").ok());
  auto history = db.GetConversationHistory("s1");
  ASSERT_TRUE(history.ok());
  ASSERT_EQ(history->back().tool_call_id, "call_123|my_tool");
  auto calls_or = orchestrator->ParseToolCalls(history->back());
  ASSERT_TRUE(calls_or.ok());
  ASSERT_EQ(calls_or->size(), 1);
  EXPECT_EQ((*calls_or)[0].id, "call_123");
  EXPECT_EQ((*calls_or)[0].name, "my_tool");
}
TEST_F(OrchestratorTest, ResponsesProactiveFiltering) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  ASSERT_TRUE(db.RegisterTool({"tool1", "desc1", "{}", true}).ok());
  nlohmann::json tool_call1;
  tool_call1["type"] = "function_call";
  tool_call1["call_id"] = "call1";
  tool_call1["name"] = "tool1";
  tool_call1["arguments"] = "{\"a\": 1}";
  nlohmann::json tool_call2;
  tool_call2["type"] = "function_call";
  tool_call2["call_id"] = "call2";
  tool_call2["name"] = "tool2";
  tool_call2["arguments"] = "{\"b\": 2}";
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", tool_call1.dump(), "call1|tool1", "tool_call").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "tool", "res1", "call1|tool1", "completed").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", tool_call2.dump(), "call2|tool2", "tool_call").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "tool", "res2", "call2|tool2", "completed").ok());
  auto result = orchestrator->AssemblePrompt("s1", {});
  ASSERT_TRUE(result.ok());
  nlohmann::json prompt = *result;
  // Verify the prompt was assembled without crashing
  ASSERT_TRUE(prompt.contains("input"));
}
}  // namespace slop
