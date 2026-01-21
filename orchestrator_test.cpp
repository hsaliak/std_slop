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
    auto orchestrator = Orchestrator::Builder(&db, &http).Build();
    
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
    auto orchestrator = Orchestrator::Builder(&db, &http).Build();
    
    Database::Skill skill = {1, "test_skill", "A test skill", "SYSTEM_PATCH"};
    ASSERT_TRUE(db.RegisterSkill(skill).ok());
    
    ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());
    
    auto result = orchestrator->AssemblePrompt("s1", {"test_skill"});
    ASSERT_TRUE(result.ok());
    
    nlohmann::json prompt = *result;
    ASSERT_TRUE(prompt.contains("system_instruction"));
    EXPECT_TRUE(prompt["system_instruction"]["parts"][0]["text"].get<std::string>().find("SYSTEM_PATCH") != std::string::npos);
}

TEST_F(OrchestratorTest, AssemblePromptWithMultipleSkills) {
    auto orchestrator = Orchestrator::Builder(&db, &http).Build();
    
    Database::Skill s1 = {0, "skill1", "desc1", "PATCH1"};
    Database::Skill s2 = {0, "skill2", "desc2", "PATCH2"};
    ASSERT_TRUE(db.RegisterSkill(s1).ok());
    ASSERT_TRUE(db.RegisterSkill(s2).ok());
    
    ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());
    
    auto result = orchestrator->AssemblePrompt("s1", {"skill1", "skill2"});
    ASSERT_TRUE(result.ok());
    
    std::string instr = (*result)["system_instruction"]["parts"][0]["text"];
    EXPECT_TRUE(instr.find("PATCH1") != std::string::npos);
    EXPECT_TRUE(instr.find("PATCH2") != std::string::npos);
}

TEST_F(OrchestratorTest, ProcessResponsePersists) {
    auto orchestrator = Orchestrator::Builder(&db, &http).Build();
    
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
    auto orchestrator = Orchestrator::Builder(&db, &http).Build();
    
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
    auto orchestrator = Orchestrator::Builder(&db, &http).Build();
    
    Database::Tool tool = {"test_tool", "A test tool", R"({"type":"object","properties":{}})", true};
    ASSERT_TRUE(db.RegisterTool(tool).ok());
    
    ASSERT_TRUE(db.AppendMessage("s1", "user", "Use the tool").ok());
    
    auto result = orchestrator->AssemblePrompt("s1");
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
    auto orchestrator = Orchestrator::Builder(&db, &http)
        .WithProvider(Orchestrator::Provider::OPENAI)
        .WithModel("gpt-4o")
        .Build();
    
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
    EXPECT_TRUE(prompt["messages"][1]["content"].get<std::string>().find("Hello") != std::string::npos);
}

TEST_F(OrchestratorTest, ProcessOpenAIResponse) {
    auto orchestrator = Orchestrator::Builder(&db, &http)
        .WithProvider(Orchestrator::Provider::OPENAI)
        .Build();
    
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

TEST_F(OrchestratorTest, ProcessOpenAIToolCall) {
    auto orchestrator = Orchestrator::Builder(&db, &http)
        .WithProvider(Orchestrator::Provider::OPENAI)
        .Build();
    
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
    auto orchestrator = Orchestrator::Builder(&db, &http)
        .WithProvider(Orchestrator::Provider::GEMINI)
        .Build();
    
    // Create invalid sequence: User -> User
    ASSERT_TRUE(db.AppendMessage("s1", "user", "Part 1").ok());
    ASSERT_TRUE(db.AppendMessage("s1", "user", "Part 2").ok());
    
    // Create orphaned tool response: User -> Tool
    ASSERT_TRUE(db.AppendMessage("s1", "tool", "{\"functionResponse\":{\"name\":\"ls\",\"response\":{\"content\":\"a.txt\"}}}").ok());

    auto result = orchestrator->AssemblePrompt("s1", {});
    ASSERT_TRUE(result.ok());
    
    nlohmann::json prompt = *result;
    // Should have 1 user turn (merged) and the tool response should be PRUNED (no preceding model call)
    ASSERT_EQ(prompt["contents"].size(), 1);
    EXPECT_EQ(prompt["contents"][0]["role"], "user");
    EXPECT_EQ(prompt["contents"][0]["parts"].size(), 2);
}

TEST_F(OrchestratorTest, ParseToolCallsGemini) {
    auto orchestrator = Orchestrator::Builder(&db, &http)
        .WithProvider(Orchestrator::Provider::GEMINI)
        .Build();
    
    Database::Message msg;
    msg.role = "assistant";
    msg.status = "tool_call";
    msg.tool_call_id = "execute_bash";
    msg.content = R"({"functionCall": {"name": "execute_bash", "args": {"command": "ls"}}})";
    
    auto tcs_or = orchestrator->ParseToolCalls(msg);
    ASSERT_TRUE(tcs_or.ok());
    ASSERT_EQ(tcs_or->size(), 1);
    EXPECT_EQ((*tcs_or)[0].name, "execute_bash");
    EXPECT_EQ((*tcs_or)[0].args["command"], "ls");
}

TEST_F(OrchestratorTest, ParseToolCallsOpenAI) {
    auto orchestrator = Orchestrator::Builder(&db, &http)
        .WithProvider(Orchestrator::Provider::OPENAI)
        .Build();
    
    Database::Message msg;
    msg.role = "assistant";
    msg.status = "tool_call";
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
    auto gemini = Orchestrator::Builder(&db, &http)
        .WithProvider(Orchestrator::Provider::GEMINI)
        .Build();
    
    ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());
    ASSERT_TRUE(db.AppendMessage("s1", "assistant", "Gemini msg", "", "completed", "g1", "gemini").ok());
    ASSERT_TRUE(db.AppendMessage("s1", "assistant", "OpenAI msg", "", "completed", "g2", "openai").ok());
    
    auto hist_or = gemini->GetRelevantHistory("s1", 0);
    ASSERT_TRUE(hist_or.ok());
    // Both Gemini and OpenAI text assistant messages are kept now.
    EXPECT_EQ(hist_or->size(), 3);
    EXPECT_EQ((*hist_or)[0].content, "Hello");
    EXPECT_EQ((*hist_or)[1].content, "Gemini msg");
    EXPECT_EQ((*hist_or)[2].content, "OpenAI msg");
    
    auto openai = Orchestrator::Builder(&db, &http)
        .WithProvider(Orchestrator::Provider::OPENAI)
        .Build();
        
    hist_or = openai->GetRelevantHistory("s1", 0);
    ASSERT_TRUE(hist_or.ok());
    // Both kept.
    EXPECT_EQ(hist_or->size(), 3);
    EXPECT_EQ((*hist_or)[0].content, "Hello");
    EXPECT_EQ((*hist_or)[1].content, "Gemini msg");
    EXPECT_EQ((*hist_or)[2].content, "OpenAI msg");
}

TEST_F(OrchestratorTest, ToolResultFiltering) {
    auto gemini = Orchestrator::Builder(&db, &http)
        .WithProvider(Orchestrator::Provider::GEMINI)
        .Build();
    
    ASSERT_TRUE(db.AppendMessage("s1", "user", "Run tool").ok());
    ASSERT_TRUE(db.AppendMessage("s1", "assistant", "call", "my_tool", "tool_call", "g1", "gemini").ok());
    ASSERT_TRUE(db.AppendMessage("s1", "tool", "result", "my_tool", "completed", "g1", "gemini").ok());
    ASSERT_TRUE(db.AppendMessage("s1", "assistant", "call", "other_tool", "tool_call", "g2", "openai").ok());
    ASSERT_TRUE(db.AppendMessage("s1", "tool", "result", "other_tool", "completed", "g2", "openai").ok());
    
    auto hist_or = gemini->GetRelevantHistory("s1", 0);
    ASSERT_TRUE(hist_or.ok());
    // Should have: "Run tool", gemini assistant call, gemini tool result. (3 messages)
    EXPECT_EQ(hist_or->size(), 3);
    for (const auto& m : *hist_or) {
        EXPECT_NE(m.parsing_strategy, "openai");
    }
}

TEST_F(OrchestratorTest, CrossModelMessagePreservation) {
    // 1. Start with Gemini
    auto gemini = Orchestrator::Builder(&db, &http)
        .WithProvider(Orchestrator::Provider::GEMINI)
        .Build();
    
    ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());
    ASSERT_TRUE(db.AppendMessage("s1", "assistant", "I am Gemini", "", "completed", "g1", "gemini").ok());
    
    // 2. Switch to OpenAI
    auto openai = Orchestrator::Builder(&db, &http)
        .WithProvider(Orchestrator::Provider::OPENAI)
        .Build();
    
    auto hist_or = openai->GetRelevantHistory("s1", 0);
    ASSERT_TRUE(hist_or.ok());
    
    // User "Hello" (text) and "I am Gemini" (text assistant) should both be kept.
    EXPECT_EQ(hist_or->size(), 2);
    EXPECT_EQ((*hist_or)[0].content, "Hello");
    EXPECT_EQ((*hist_or)[1].content, "I am Gemini");

    // 3. Add a Gemini Tool Call (should be filtered for OpenAI)
    ASSERT_TRUE(db.AppendMessage("s1", "assistant", "{}", "tool_1", "tool_call", "g2", "gemini").ok());
    hist_or = openai->GetRelevantHistory("s1", 0);
    ASSERT_EQ(hist_or->size(), 2); // Still 2
}

TEST_F(OrchestratorTest, ProcessResponseExtractsUsageGemini) {
    auto orchestrator = Orchestrator::Builder(&db, &http)
        .WithProvider(Orchestrator::Provider::GEMINI)
        .WithModel("gemini-1.5-pro")
        .Build();

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
    auto orchestrator = Orchestrator::Builder(&db, &http)
        .WithProvider(Orchestrator::Provider::OPENAI)
        .WithModel("gpt-4o")
        .Build();

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

}  // namespace slop
