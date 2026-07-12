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
  ASSERT_EQ(prompt["contents"].size(), 2);
  EXPECT_EQ(prompt["contents"][0]["role"], "user");
  EXPECT_EQ(prompt["contents"][1]["role"], "model");
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
  ASSERT_TRUE(prompt.contains("system_instruction"));
  EXPECT_TRUE(absl::StrContains(prompt["system_instruction"]["parts"][0]["text"].get<std::string>(), "SYSTEM_PATCH"));
}
TEST_F(OrchestratorTest, AssemblePromptWithMultipleSkills) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  Database::Skill s1 = {0, "skill1", "desc1", "PATCH1"};
  Database::Skill s2 = {0, "skill2", "desc2", "PATCH2"};
  ASSERT_TRUE(db.RegisterSkill(s1).ok());
  ASSERT_TRUE(db.RegisterSkill(s2).ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());
  auto result = orchestrator->AssemblePrompt("s1", {"skill1", "skill2"});
  ASSERT_TRUE(result.ok());
  std::string instr = (*result)["system_instruction"]["parts"][0]["text"];
  EXPECT_TRUE(absl::StrContains(instr, "PATCH1"));
  EXPECT_TRUE(absl::StrContains(instr, "PATCH2"));
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
  for (const auto& content : prompt.value("contents", nlohmann::json::array())) {
    for (const auto& part : content.value("parts", nlohmann::json::array())) {
      if (part.contains("functionResponse")) {
        std::string tool_content = part.value("functionResponse", nlohmann::json::object())
                                       .value("response", nlohmann::json::object())
                                       .value("content", "");
        if (tool_content == long_content) {
          if (!found_g1) {
            found_g1 = true;
          } else {
            found_g2 = true;
          }
        }
      }
    }
  }
  EXPECT_TRUE(found_g1);
  EXPECT_TRUE(found_g2);
}
TEST_F(OrchestratorTest, AccordionToolResultsUseFullFidelityLimit) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::GEMINI).Build();
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
  for (const auto& content : prompt.value("contents", nlohmann::json::array())) {
    for (const auto& part : content.value("parts", nlohmann::json::array())) {
      if (part.contains("functionResponse")) {
        std::string tool_content = part.value("functionResponse", nlohmann::json::object())
                                       .value("response", nlohmann::json::object())
                                       .value("content", "");
        if (absl::StrContains(tool_content, "TRUNCATED. Use query_db")) {
          EXPECT_LT(tool_content.size(), ts.full_fidelity_limit + 200);
          truncated_count++;
        } else if (tool_content == long_content) {
          full_fidelity_count++;
        }
      }
    }
  }
  EXPECT_EQ(truncated_count, total_tools);
  EXPECT_EQ(full_fidelity_count, 0);
}
TEST_F(OrchestratorTest, AccordionPreservesToolResultsBelowFullFidelityLimit) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::OPENAI).Build();
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
  // OpenAI payload structure: messages -> role: tool, content
  bool found_g1 = false;
  bool found_g2 = false;
  for (const auto& msg : prompt.value("messages", nlohmann::json::array())) {
    if (msg.value("role", "") == "tool") {
      std::string tool_content = msg.value("content", "");
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
        "candidates": [{
            "content": {
                "parts": [{"text": "I am an AI assistant"}]
            }
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
  std::string mock_response = R"({
        "candidates": [{
            "content": {
                "parts": [{
                    "functionCall": {
                        "name": "execute_bash",
                        "args": {"command": "ls"}
                    }
                }]
            }
        }]
    })";
  ASSERT_TRUE(orchestrator->ProcessResponse("s1", mock_response).ok());
  auto history = db.GetConversationHistory("s1");
  ASSERT_TRUE(history.ok());
  ASSERT_EQ(history->size(), 1);
  EXPECT_EQ((*history)[0].role, "assistant");
  EXPECT_EQ((*history)[0].status, "tool_call");
  nlohmann::json content = nlohmann::json::parse((*history)[0].content, nullptr, false);
  ASSERT_FALSE(content.is_discarded());
  EXPECT_EQ(content["functionCall"]["name"], "execute_bash");
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
  ASSERT_TRUE(prompt["tools"][0].contains("function_declarations"));
  bool found = false;
  for (const auto& decl : prompt["tools"][0]["function_declarations"]) {
    if (decl["name"] == "query_db") {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(OrchestratorTest, GeminiToolSchemasNormalizeUnionTypes) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::GEMINI).Build();
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
  ASSERT_TRUE(prompt["tools"][0].contains("function_declarations"));
  const nlohmann::json* decl = nullptr;
  for (const auto& d : prompt["tools"][0]["function_declarations"]) {
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
  EXPECT_TRUE(parameters["properties"]["index"]["type"].is_string());
  EXPECT_EQ(parameters["properties"]["index"]["type"], "integer");
}

TEST_F(OrchestratorTest, AssembleOpenAIPrompt) {
  auto orchestrator_or =
      Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::OPENAI).WithModel("gpt-4o").Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());
  auto result = orchestrator->AssemblePrompt("s1", {});
  ASSERT_TRUE(result.ok());
  nlohmann::json prompt = *result;
  EXPECT_EQ(prompt["model"], "gpt-4o");
  ASSERT_TRUE(prompt.contains("messages"));
  // First message is the default system prompt
  EXPECT_EQ(prompt["messages"][0]["role"], "system");
  // Second message is the user prompt
  EXPECT_EQ(prompt["messages"][1]["role"], "user");
  EXPECT_TRUE(absl::StrContains(prompt["messages"][1]["content"].get<std::string>(), "Hello"));
}
TEST_F(OrchestratorTest, ProcessOpenAIResponse) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::OPENAI).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  std::string mock_response = R"({
        "choices": [{
            "message": {
                "role": "assistant",
                "content": "Hello from OpenAI"
            }
        }]
    })";
  ASSERT_TRUE(orchestrator->ProcessResponse("s1", mock_response).ok());
  auto history = db.GetConversationHistory("s1");
  ASSERT_TRUE(history.ok());
  ASSERT_EQ(history->size(), 1);
  EXPECT_EQ((*history)[0].content, "Hello from OpenAI");
}
TEST_F(OrchestratorTest, ProcessResponseKeepsStateOnlyInHistory) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  const std::string assistant_state = "Hello!\n\n### STATE\nGoal: unit test\nContext: none";
  const std::string mock_response = absl::StrCat(
      R"({"candidates":[{"content":{"parts":[{"text":)", nlohmann::json(assistant_state).dump(), R"(}]}}]})");
  ASSERT_TRUE(orchestrator->ProcessResponse("s1", mock_response).ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "next request").ok());
  auto prompt_or = orchestrator->AssemblePrompt("s1");
  ASSERT_TRUE(prompt_or.ok());
  const std::string system_instruction = (*prompt_or)["system_instruction"]["parts"][0]["text"];
  EXPECT_FALSE(absl::StrContains(system_instruction, "## Global State (Anchor)"));
  auto history_or = db.GetConversationHistory("s1");
  ASSERT_TRUE(history_or.ok());
  ASSERT_EQ(history_or->size(), 2);
  EXPECT_EQ((*history_or)[0].content, assistant_state);
  const std::string serialized_state = nlohmann::json(assistant_state).dump();
  const std::string prompt_json = prompt_or->dump();
  EXPECT_EQ(prompt_json.find(serialized_state), prompt_json.rfind(serialized_state));
}
TEST_F(OrchestratorTest, ProcessOpenAIToolCall) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::OPENAI).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  std::string mock_response = R"({
        "choices": [{
            "message": {
                "role": "assistant",
                "tool_calls": [{
                    "id": "call_abc",
                    "type": "function",
                    "function": {
                        "name": "execute_bash",
                        "arguments": "{\"command\":\"ls\"}"
                    }
                }]
            }
        }]
    })";
  ASSERT_TRUE(orchestrator->ProcessResponse("s1", mock_response).ok());
  auto history = db.GetConversationHistory("s1");
  ASSERT_TRUE(history.ok());
  ASSERT_FALSE(history->empty());
  EXPECT_EQ((*history)[0].status, "tool_call");
  EXPECT_EQ((*history)[0].tool_call_id, "call_abc|execute_bash");
}
TEST_F(OrchestratorTest, GeminiHistoryNormalization) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::GEMINI).Build();
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
  ASSERT_EQ(prompt["contents"].size(), 1);
  EXPECT_EQ(prompt["contents"][0]["role"], "user");
  EXPECT_EQ(prompt["contents"][0]["parts"].size(), 3);
}
TEST_F(OrchestratorTest, ParseToolCallsGemini) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::GEMINI).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  Database::Message msg;
  msg.role = "assistant";
  msg.status = "tool_call";
  msg.parsing_strategy = "gemini";
  msg.tool_call_id = "execute_bash";
  msg.content = R"({"functionCall": {"name": "execute_bash", "args": {"command": "ls"}}})";
  auto tcs_or = orchestrator->ParseToolCalls(msg);
  ASSERT_TRUE(tcs_or.ok());
  ASSERT_EQ(tcs_or->size(), 1);
  EXPECT_EQ((*tcs_or)[0].name, "execute_bash");
  EXPECT_EQ((*tcs_or)[0].args["command"], "ls");
}
TEST_F(OrchestratorTest, ParseToolCallsOpenAI) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::OPENAI).Build();
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
  auto gemini_or = Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::GEMINI).Build();
  ASSERT_TRUE(gemini_or.ok());
  auto gemini = std::move(*gemini_or);
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello", "", "completed", "g0").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "Gemini msg", "", "completed", "g1", "gemini").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "OpenAI msg", "", "completed", "g2", "openai").ok());
  auto hist_or = gemini->GetAccordionHistory("s1");
  ASSERT_TRUE(hist_or.ok());
  // Both Gemini and OpenAI text assistant messages are kept now.
  EXPECT_EQ(hist_or->size(), 3);
  EXPECT_EQ((*hist_or)[0].content, "Hello");
  EXPECT_EQ((*hist_or)[1].content, "Gemini msg");
  EXPECT_EQ((*hist_or)[2].content, "OpenAI msg");
  auto openai_or = Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::OPENAI).Build();
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
  auto gemini_or = Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::GEMINI).Build();
  ASSERT_TRUE(gemini_or.ok());
  auto gemini = std::move(*gemini_or);
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Run tool", "", "completed", "g0").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "call", "my_tool", "tool_call", "g1", "gemini").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "tool", "result", "my_tool", "completed", "g1", "gemini").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "call", "other_tool", "tool_call", "g2", "openai").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "tool", "result", "other_tool", "completed", "g2", "openai").ok());
  auto hist_or = gemini->GetAccordionHistory("s1");
  ASSERT_TRUE(hist_or.ok());
  // Should have: "Run tool", gemini assistant call, gemini tool result. (3 messages)
  EXPECT_EQ(hist_or->size(), 3);
  for (const auto& m : *hist_or) {
    EXPECT_NE(m.parsing_strategy, "openai");
  }
}
TEST_F(OrchestratorTest, CrossModelMessagePreservation) {
  // 1. Start with Gemini
  auto gemini_or = Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::GEMINI).Build();
  ASSERT_TRUE(gemini_or.ok());
  auto gemini = std::move(*gemini_or);
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello", "", "completed", "g0").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "I am Gemini", "", "completed", "g1", "gemini").ok());
  // 2. Switch to OpenAI
  auto openai_or = Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::OPENAI).Build();
  ASSERT_TRUE(openai_or.ok());
  auto openai = std::move(*openai_or);
  auto hist_or = openai->GetAccordionHistory("s1");
  ASSERT_TRUE(hist_or.ok());
  // User "Hello" (text) and "I am Gemini" (text assistant) should both be kept.
  EXPECT_EQ(hist_or->size(), 2);
  EXPECT_EQ((*hist_or)[0].content, "Hello");
  EXPECT_EQ((*hist_or)[1].content, "I am Gemini");
  // 3. Add a Gemini Tool Call (should be filtered for OpenAI)
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "{}", "tool_1", "tool_call", "g2", "gemini").ok());
  hist_or = openai->GetAccordionHistory("s1");
  ASSERT_EQ(hist_or->size(), 2);  // Still 2
}
TEST_F(OrchestratorTest, ProcessResponseExtractsUsageGemini) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http)
                             .WithProvider(Orchestrator::Provider::GEMINI)
                             .WithModel("gemini-1.5-pro")
                             .Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  std::string mock_response = R"({
        "candidates": [{"content": {"parts": [{"text": "Hello"}]}}],
        "usageMetadata": {
            "promptTokenCount": 10,
            "candidatesTokenCount": 5
        }
    })";
  ASSERT_TRUE(orchestrator->ProcessResponse("s1", mock_response).ok());
  auto usage_or = db.GetTotalUsage("s1");
  ASSERT_TRUE(usage_or.ok());
  EXPECT_EQ(usage_or->prompt_tokens, 10);
  EXPECT_EQ(usage_or->completion_tokens, 5);
}
TEST_F(OrchestratorTest, ProcessResponseExtractsUsageOpenAI) {
  auto orchestrator_or =
      Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::OPENAI).WithModel("gpt-4o").Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  std::string mock_response = R"({
        "choices": [{"message": {"role": "assistant", "content": "Hello"}}],
        "usage": {
            "prompt_tokens": 20,
            "completion_tokens": 10
        }
    })";
  ASSERT_TRUE(orchestrator->ProcessResponse("s1", mock_response).ok());
  auto usage_or = db.GetTotalUsage("s1");
  ASSERT_TRUE(usage_or.ok());
  EXPECT_EQ(usage_or->prompt_tokens, 20);
  EXPECT_EQ(usage_or->completion_tokens, 10);
}
TEST_F(OrchestratorTest, GeminiDoesNotIncludeTransforms) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::GEMINI).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  auto result = orchestrator->AssemblePrompt("s1");
  ASSERT_TRUE(result.ok());
  nlohmann::json prompt = *result;
  EXPECT_FALSE(prompt.contains("transforms"));
}
TEST_F(OrchestratorTest, GeminiMultiToolCallProcessing) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::GEMINI).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  // Mock response with two function calls
  std::string mock_response = R"({
    "candidates": [{
      "content": {
        "parts": [
          {"functionCall": {"name": "tool1", "args": {"a": 1}}},
          {"functionCall": {"name": "tool2", "args": {"b": 2}}}
        ]
      }
    }]
  })";
  ASSERT_TRUE(orchestrator->ProcessResponse("s1", mock_response, "g1").ok());
  auto history = db.GetConversationHistory("s1");
  ASSERT_TRUE(history.ok());
  // Should have 2 assistant messages in the same group
  int count = 0;
  for (const auto& msg : *history) {
    if (msg.role == "assistant" && msg.status == "tool_call") {
      count++;
    }
  }
  EXPECT_EQ(count, 2);
}
TEST_F(OrchestratorTest, GeminiMultiToolResponseAssembly) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::GEMINI).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  // Register tools so they aren't filtered out
  ASSERT_TRUE(db.RegisterTool({"tool1", "desc1", "{}", true}).ok());
  ASSERT_TRUE(db.RegisterTool({"tool2", "desc2", "{}", true}).ok());
  // 1. Add user message
  ASSERT_TRUE(db.AppendMessage("s1", "user", "run tools", "", "completed", "g1").ok());
  // 2. Add assistant multi-tool call (simulating what ProcessResponse would do)
  // Part 1
  nlohmann::json call1 = {{"functionCall", {{"name", "tool1"}, {"args", {{"a", 1}}}}}};
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", call1.dump(), "tool1", "tool_call", "g2", "gemini").ok());
  // Part 2
  nlohmann::json call2 = {{"functionCall", {{"name", "tool2"}, {"args", {{"b", 2}}}}}};
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", call2.dump(), "tool2", "tool_call", "g2", "gemini").ok());
  // 3. Add tool responses
  ASSERT_TRUE(db.AppendMessage("s1", "tool", "result1", "tool1", "completed", "g2", "gemini").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "tool", "result2", "tool2", "completed", "g2", "gemini").ok());
  // 4. Assemble prompt
  auto result = orchestrator->AssemblePrompt("s1");
  ASSERT_TRUE(result.ok());
  nlohmann::json prompt = *result;
  // contents should have:
  // 0: user (text)
  // 1: model (2 parts: tool1 call, tool2 call)
  // 2: function (2 parts: tool1 response, tool2 response)
  ASSERT_EQ(prompt["contents"].size(), 3);
  EXPECT_EQ(prompt["contents"][1]["role"], "model");
  ASSERT_EQ(prompt["contents"][1]["parts"].size(), 2);
  EXPECT_TRUE(prompt["contents"][1]["parts"][0].contains("functionCall"));
  EXPECT_EQ(prompt["contents"][1]["parts"][0]["functionCall"]["name"], "tool1");
  EXPECT_TRUE(prompt["contents"][1]["parts"][1].contains("functionCall"));
  EXPECT_EQ(prompt["contents"][1]["parts"][1]["functionCall"]["name"], "tool2");
  EXPECT_EQ(prompt["contents"][2]["role"], "function");
  ASSERT_EQ(prompt["contents"][2]["parts"].size(), 2);
  EXPECT_TRUE(prompt["contents"][2]["parts"][0].contains("functionResponse"));
  EXPECT_EQ(prompt["contents"][2]["parts"][0]["functionResponse"]["name"], "tool1");
  EXPECT_TRUE(prompt["contents"][2]["parts"][1].contains("functionResponse"));
  EXPECT_EQ(prompt["contents"][2]["parts"][1]["functionResponse"]["name"], "tool2");
}
TEST_F(OrchestratorTest, OpenAIIdNameHandling) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::OPENAI).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  // Mock response with a tool call
  std::string mock_response = R"({
      "choices": [{
        "message": {
          "role": "assistant",
          "tool_calls": [{
            "id": "call_123",
            "type": "function",
            "function": {"name": "my_tool", "arguments": "{}"}
          }]
        }
      }]
    })";
  ASSERT_TRUE(orchestrator->ProcessResponse("s1", mock_response, "g1").ok());
  auto history = db.GetConversationHistory("s1");
  ASSERT_TRUE(history.ok());
  ASSERT_EQ(history->back().tool_call_id, "call_123|my_tool");
  // Test parsing it back
  auto calls_or = orchestrator->ParseToolCalls(history->back());
  ASSERT_TRUE(calls_or.ok());
  ASSERT_EQ(calls_or->size(), 1);
  EXPECT_EQ((*calls_or)[0].id, "call_123");
  EXPECT_EQ((*calls_or)[0].name, "my_tool");
}
TEST_F(OrchestratorTest, GeminiProactiveFiltering) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).WithProvider(Orchestrator::Provider::GEMINI).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto orchestrator = std::move(*orchestrator_or);
  // Register only "tool1"
  ASSERT_TRUE(db.RegisterTool({"tool1", "desc1", "{}", true}).ok());
  // Add "tool1" (valid) and "tool2" (invalid) calls
  nlohmann::json tool_call1 = {{"functionCall", {{"name", "tool1"}, {"args", {{"a", 1}}}}}};
  nlohmann::json tool_call2 = {{"functionCall", {{"name", "tool2"}, {"args", {{"b", 2}}}}}};
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", tool_call1.dump(), "call1|tool1", "tool_call").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "tool", "res1", "call1|tool1", "completed").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", tool_call2.dump(), "call2|tool2", "tool_call").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "tool", "res2", "call2|tool2", "completed").ok());
  auto result = orchestrator->AssemblePrompt("s1", {});
  ASSERT_TRUE(result.ok());
  nlohmann::json prompt = *result;
  // Index 0: model (tool1 call)
  EXPECT_EQ(prompt["contents"][0]["role"], "model");
  EXPECT_TRUE(prompt["contents"][0]["parts"][0].contains("functionCall"));
  // Index 1: function (tool1 response)
  EXPECT_EQ(prompt["contents"][1]["role"], "function");
  EXPECT_TRUE(prompt["contents"][1]["parts"][0].contains("functionResponse"));
  // Index 2: model (tool2 call - suppressed)
  EXPECT_EQ(prompt["contents"][2]["role"], "model");
  EXPECT_TRUE(prompt["contents"][2]["parts"][0].contains("text"));
  EXPECT_FALSE(prompt["contents"][2]["parts"][0].contains("functionCall"));
  // Index 3: user (tool2 response - suppressed and role changed)
  EXPECT_EQ(prompt["contents"][3]["role"], "user");
  EXPECT_TRUE(prompt["contents"][3]["parts"][0].contains("text"));
}
}  // namespace slop
