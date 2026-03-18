
#include "core/orchestrator_gemini.h"
#include "core/orchestrator_openai.h"
#include "core/orchestrator_openai_responses.h"

#include <string>
#include <tuple>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop {
namespace {

void OpenAiProcessResponseNeverCrashes(const std::string& response_json) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  HttpClient http;
  OpenAiOrchestrator orchestrator(&db, &http, "gpt-4o", "https://api.openai.com/v1");

  auto result = orchestrator.ProcessResponse("s1", response_json, "g1");
  if (!result.ok()) {
    EXPECT_FALSE(result.ok());
    return;
  }
  EXPECT_GE(*result, 0);
}

void GeminiProcessResponseNeverCrashes(const std::string& response_json) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  HttpClient http;
  GeminiOrchestrator orchestrator(&db, &http, "gemini-2.0-flash", "https://generativelanguage.googleapis.com/v1beta");

  auto result = orchestrator.ProcessResponse("s1", response_json, "g1");
  if (!result.ok()) {
    EXPECT_FALSE(result.ok());
    return;
  }
  EXPECT_GE(*result, 0);
}

void OpenAiResponsesNormalizationNeverCrashes(const std::string& response_json) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  HttpClient http;
  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-4o", "https://api.openai.com/v1");

  auto result = orchestrator.ProcessResponse("s1", response_json, "g1");
  if (!result.ok()) {
    EXPECT_FALSE(result.ok());
    return;
  }
  EXPECT_GE(*result, 0);
}

FUZZ_TEST(OrchestratorNormalizationFuzzTest, OpenAiProcessResponseNeverCrashes)
    .WithSeeds(std::vector<std::tuple<std::string>>{
        std::make_tuple(std::string(R"({
          "choices": [{"message": {"content": "Hello from OpenAI"}}],
          "usage": {"prompt_tokens": 1, "completion_tokens": 2, "total_tokens": 3}
        })")),
        std::make_tuple(std::string(
            R"({"choices":[{"message":{"tool_calls":[{"id":"call_1","function":{"name":"query_db","arguments":"{\"sql\":\"SELECT 1\"}"}}]}}]})")),
    });

FUZZ_TEST(OrchestratorNormalizationFuzzTest, GeminiProcessResponseNeverCrashes)
    .WithSeeds(std::vector<std::tuple<std::string>>{
        std::make_tuple(std::string(R"({
          "candidates": [{"content": {"parts": [{"text": "I am an AI assistant"}]}}],
          "usageMetadata": {"promptTokenCount": 1, "candidatesTokenCount": 2, "totalTokenCount": 3}
        })")),
        std::make_tuple(std::string(
            R"({"candidates":[{"content":{"parts":[{"functionCall":{"name":"read_file","args":{"path":"AGENTS.md"}}}]}}]})")),
    });

FUZZ_TEST(OrchestratorNormalizationFuzzTest, OpenAiResponsesNormalizationNeverCrashes)
    .WithSeeds(std::vector<std::tuple<std::string>>{
        std::make_tuple(std::string(R"({
          "usage": { "input_tokens": 3, "output_tokens": 4 },
          "output": [{"type": "message", "content": [{"type": "output_text", "text": "Hello from responses"}]}]
        })")),
        std::make_tuple(std::string("event: response.output_item.done\n"
                                    "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"function_call\","
                                    "\"call_id\":\"call_1\",\"name\":\"query_db\","
                                    "\"arguments\":\"{\\\"sql\\\":\\\"SELECT 7\\\"}\"}}\n\n"
                                    "event: response.completed\n"
                                    "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp1\"}}\n\n")),
    });

}  // namespace
}  // namespace slop