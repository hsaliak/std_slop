#include "orchestrator.h"
#include "database.h"
#include <gtest/gtest.h>

namespace slop {

class OrchestratorTest : public ::testing::Test {
 protected:
  Database db;
  HttpClient http;
  
  void SetUp() override {
    ASSERT_TRUE(db.Init(":memory:").ok());
  }
};

TEST_F(OrchestratorTest, AssemblePromptBasic) {
    Orchestrator orchestrator(&db, &http);
    
    ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());
    ASSERT_TRUE(db.AppendMessage("s1", "assistant", "Hi!").ok());
    
    auto result = orchestrator.AssemblePrompt("s1", {});
    ASSERT_TRUE(result.ok());
    
    nlohmann::json prompt = *result;
    ASSERT_EQ(prompt["contents"].size(), 2);
    EXPECT_EQ(prompt["contents"][0]["role"], "user");
    EXPECT_EQ(prompt["contents"][1]["role"], "model");
}

TEST_F(OrchestratorTest, AssemblePromptWithSkills) {
    Orchestrator orchestrator(&db, &http);
    
    Database::Skill skill = {1, "test_skill", "A test skill", "SYSTEM_PATCH"};
    ASSERT_TRUE(db.RegisterSkill(skill).ok());
    
    ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());
    
    auto result = orchestrator.AssemblePrompt("s1", {"test_skill"});
    ASSERT_TRUE(result.ok());
    
    nlohmann::json prompt = *result;
    ASSERT_TRUE(prompt.contains("system_instruction"));
    EXPECT_TRUE(prompt["system_instruction"]["parts"][0]["text"].get<std::string>().find("SYSTEM_PATCH") != std::string::npos);
}

TEST_F(OrchestratorTest, ProcessResponsePersists) {
    Orchestrator orchestrator(&db, &http);
    
    std::string mock_response = R"({
        "candidates": [{
            "content": {
                "parts": [{"text": "I am an AI assistant"}]
            }
        }]
    })";
    
    ASSERT_TRUE(orchestrator.ProcessResponse("s1", mock_response).ok());
    
    auto history = db.GetConversationHistory("s1");
    ASSERT_TRUE(history.ok());
    ASSERT_EQ(history->size(), 1);
    EXPECT_EQ((*history)[0].role, "assistant");
    EXPECT_EQ((*history)[0].content, "I am an AI assistant");
}

TEST_F(OrchestratorTest, ProcessResponseToolCall) {
    Orchestrator orchestrator(&db, &http);
    
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
    
    ASSERT_TRUE(orchestrator.ProcessResponse("s1", mock_response).ok());
    
    auto history = db.GetConversationHistory("s1");
    ASSERT_TRUE(history.ok());
    ASSERT_EQ(history->size(), 1);
    EXPECT_EQ((*history)[0].role, "assistant");
    EXPECT_EQ((*history)[0].status, "tool_call");
    
    nlohmann::json content = nlohmann::json::parse((*history)[0].content);
    EXPECT_EQ(content["functionCall"]["name"], "execute_bash");
}

TEST_F(OrchestratorTest, AssemblePromptWithTools) {
    Orchestrator orchestrator(&db, &http);
    
    Database::Tool tool = {"test_tool", "A test tool", R"({"type":"object","properties":{}})", true};
    ASSERT_TRUE(db.RegisterTool(tool).ok());
    
    ASSERT_TRUE(db.AppendMessage("s1", "user", "Use the tool").ok());
    
    auto result = orchestrator.AssemblePrompt("s1");
    ASSERT_TRUE(result.ok());
    
    nlohmann::json prompt = *result;
    ASSERT_TRUE(prompt.contains("tools"));
    ASSERT_TRUE(prompt["tools"][0].contains("function_declarations"));
    
    bool found = false;
    for (const auto& decl : prompt["tools"][0]["function_declarations"]) {
        if (decl["name"] == "test_tool") { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST_F(OrchestratorTest, AssembleOpenAIPrompt) {
    Orchestrator orchestrator(&db, &http);
    orchestrator.SetProvider(Orchestrator::Provider::OPENAI);
    orchestrator.SetModel("gpt-4o");
    
    ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());
    
    auto result = orchestrator.AssemblePrompt("s1", {});
    ASSERT_TRUE(result.ok());
    
    nlohmann::json prompt = *result;
    EXPECT_EQ(prompt["model"], "gpt-4o");
    ASSERT_TRUE(prompt.contains("messages"));
    // First message is the default system prompt
    EXPECT_EQ(prompt["messages"][0]["role"], "system");
    // Second message is the user prompt
    EXPECT_EQ(prompt["messages"][1]["role"], "user");
    EXPECT_EQ(prompt["messages"][1]["content"], "Hello");
}

TEST_F(OrchestratorTest, ProcessOpenAIResponse) {
    Orchestrator orchestrator(&db, &http);
    orchestrator.SetProvider(Orchestrator::Provider::OPENAI);
    
    std::string mock_response = R"({
        "choices": [{
            "message": {
                "role": "assistant",
                "content": "Hello from OpenAI"
            }
        }]
    })";
    
    ASSERT_TRUE(orchestrator.ProcessResponse("s1", mock_response).ok());
    
    auto history = db.GetConversationHistory("s1");
    ASSERT_TRUE(history.ok());
    ASSERT_EQ(history->size(), 1);
    EXPECT_EQ((*history)[0].content, "Hello from OpenAI");
}

TEST_F(OrchestratorTest, ProcessOpenAIToolCall) {
    Orchestrator orchestrator(&db, &http);
    orchestrator.SetProvider(Orchestrator::Provider::OPENAI);
    
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
    
    ASSERT_TRUE(orchestrator.ProcessResponse("s1", mock_response).ok());
    
    auto history = db.GetConversationHistory("s1");
    ASSERT_TRUE(history.ok());
    ASSERT_FALSE(history->empty());
    EXPECT_EQ((*history)[0].status, "tool_call");
    EXPECT_EQ((*history)[0].tool_call_id, "call_abc|execute_bash");
}

TEST_F(OrchestratorTest, GeminiHistoryNormalization) {
    Orchestrator orchestrator(&db, &http);
    orchestrator.SetProvider(Orchestrator::Provider::GEMINI);
    
    // Create invalid sequence: User -> User
    ASSERT_TRUE(db.AppendMessage("s1", "user", "Part 1").ok());
    ASSERT_TRUE(db.AppendMessage("s1", "user", "Part 2").ok());
    
    // Create orphaned tool response: User -> Tool
    ASSERT_TRUE(db.AppendMessage("s1", "tool", "{\"functionResponse\":{\"name\":\"ls\",\"response\":{\"content\":\"a.txt\"}}}").ok());

    auto result = orchestrator.AssemblePrompt("s1", {});
    ASSERT_TRUE(result.ok());
    
    nlohmann::json prompt = *result;
    // Should have 1 user turn (merged) and the tool response should be PRUNED (no preceding model call)
    ASSERT_EQ(prompt["contents"].size(), 1);
    EXPECT_EQ(prompt["contents"][0]["role"], "user");
    EXPECT_EQ(prompt["contents"][0]["parts"].size(), 2);
}

TEST_F(OrchestratorTest, SkipsMalformedTools) {
    Orchestrator orchestrator(&db, &http);
    
    // Valid tool
    Database::Tool valid = {"valid", "desc", R"({"type":"object"})", true};
    ASSERT_TRUE(db.RegisterTool(valid).ok());
    
    // Malformed tool (invalid JSON)
    Database::Tool invalid = {"invalid", "desc", R"({"type": "object", broken})", true};
    ASSERT_TRUE(db.RegisterTool(invalid).ok());
    
    ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());
    
    auto result = orchestrator.AssemblePrompt("s1");
    ASSERT_TRUE(result.ok());
    
    nlohmann::json prompt = *result;
    ASSERT_TRUE(prompt.contains("tools"));
    
    // For Gemini format
    auto& decls = prompt["tools"][0]["function_declarations"];
    bool found_valid = false;
    bool found_invalid = false;
    for (const auto& d : decls) {
        if (d["name"] == "valid") found_valid = true;
        if (d["name"] == "invalid") found_invalid = true;
    }
    EXPECT_TRUE(found_valid);
    EXPECT_FALSE(found_invalid);

    // Test for OpenAI
    orchestrator.SetProvider(Orchestrator::Provider::OPENAI);
    result = orchestrator.AssemblePrompt("s1");
    ASSERT_TRUE(result.ok());
    prompt = *result;
    ASSERT_TRUE(prompt.contains("tools"));
    
    found_valid = false;
    found_invalid = false;
    for (const auto& d : prompt["tools"]) {
        if (d["function"]["name"] == "valid") found_valid = true;
        if (d["function"]["name"] == "invalid") found_invalid = true;
    }
    EXPECT_TRUE(found_valid);
    EXPECT_FALSE(found_invalid);
}

TEST_F(OrchestratorTest, ParseToolCallGemini) {
    Orchestrator orchestrator(&db, &http);
    orchestrator.SetProvider(Orchestrator::Provider::GEMINI);
    
    Database::Message msg;
    msg.role = "assistant";
    msg.status = "tool_call";
    msg.tool_call_id = "test_tool";
    msg.content = R"({"functionCall": {"name": "test_tool", "args": {"key": "val"}}})";
    
    auto tc = orchestrator.ParseToolCall(msg);
    ASSERT_TRUE(tc.ok());
    EXPECT_EQ(tc->name, "test_tool");
    EXPECT_EQ(tc->args["key"], "val");
}

TEST_F(OrchestratorTest, ParseToolCallOpenAI) {
    Orchestrator orchestrator(&db, &http);
    orchestrator.SetProvider(Orchestrator::Provider::OPENAI);
    
    Database::Message msg;
    msg.role = "assistant";
    msg.status = "tool_call";
    msg.tool_call_id = "call_123|test_tool";
    msg.content = R"({"tool_calls": [{"id": "call_123", "type": "function", "function": {"name": "test_tool", "arguments": "{\"key\": \"val\"}"}}]})";
    
    auto tc = orchestrator.ParseToolCall(msg);
    ASSERT_TRUE(tc.ok());
    EXPECT_EQ(tc->name, "test_tool");
    EXPECT_EQ(tc->id, "call_123");
    EXPECT_EQ(tc->args["key"], "val");
}

TEST_F(OrchestratorTest, GcaPayloadWrapping) {
    Orchestrator orchestrator(&db, &http);
    orchestrator.SetModel("gemini-3-flash-preview");
    orchestrator.SetGcaMode(true);
    orchestrator.SetProjectId("test-proj");
    
    ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());
    
    auto result = orchestrator.AssemblePrompt("s1");
    ASSERT_TRUE(result.ok());
    
    nlohmann::json prompt = *result;
    EXPECT_EQ(prompt["model"], "gemini-3-flash-preview");
    EXPECT_EQ(prompt["project"], "test-proj");
    ASSERT_TRUE(prompt.contains("user_prompt_id"));
    ASSERT_TRUE(prompt.contains("request"));
    EXPECT_EQ(prompt["request"]["contents"][0]["parts"][0]["text"], "Hello");
    EXPECT_EQ(prompt["request"]["session_id"], "s1");
}

TEST_F(OrchestratorTest, GcaResponseUnwrapping) {
    Orchestrator orchestrator(&db, &http);
    orchestrator.SetGcaMode(true);
    
    std::string mock_gca_response = R"({
        "response": {
            "candidates": [{
                "content": {
                    "parts": [{"text": "Hello from GCA"}]
                }
            }]
        }
    })";
    
    ASSERT_TRUE(orchestrator.ProcessResponse("s1", mock_gca_response).ok());
    
    auto history = db.GetConversationHistory("s1");
    ASSERT_TRUE(history.ok());
    ASSERT_EQ(history->size(), 1);
    EXPECT_EQ((*history)[0].content, "Hello from GCA");
}

TEST_F(OrchestratorTest, ProcessResponseExtractsUsageGemini) {
    Orchestrator orchestrator(&db, &http);
    orchestrator.SetModel("test-gemini");
    
    std::string mock_response = R"({
        "candidates": [{"content": {"parts": [{"text": "Hello"}]}}],
        "usageMetadata": {
            "promptTokenCount": 10,
            "candidatesTokenCount": 5
        }
    })";
    
    ASSERT_TRUE(orchestrator.ProcessResponse("s1", mock_response).ok());
    
    auto usage = db.GetTotalUsage("s1");
    ASSERT_TRUE(usage.ok());
    EXPECT_EQ(usage->prompt_tokens, 10);
    EXPECT_EQ(usage->completion_tokens, 5);
}

TEST_F(OrchestratorTest, ProcessResponseExtractsUsageOpenAI) {
    Orchestrator orchestrator(&db, &http);
    orchestrator.SetProvider(Orchestrator::Provider::OPENAI);
    orchestrator.SetModel("test-openai");
    
    std::string mock_response = R"({
        "choices": [{"message": {"role": "assistant", "content": "Hello"}}],
        "usage": {
            "prompt_tokens": 20,
            "completion_tokens": 10
        }
    })";
    
    ASSERT_TRUE(orchestrator.ProcessResponse("s1", mock_response).ok());
    
    auto usage = db.GetTotalUsage("s1");
    ASSERT_TRUE(usage.ok());
    EXPECT_EQ(usage->prompt_tokens, 20);
    EXPECT_EQ(usage->completion_tokens, 10);
}

}  // namespace slop
