
#include "core/orchestrator_openai_responses.h"

#include <string>
#include <tuple>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop {
namespace {



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

void StoredResponsesItemNeverCrashes(const std::string& api_item_json) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  HttpClient http;
  OpenAiResponsesOrchestrator orchestrator(&db, &http, "gpt-4o", "https://api.openai.com/v1");
  Database::Message message{0, "s1", "assistant", "", "", "provider_item", "", "g1", "openai", 0, api_item_json};

  auto result = orchestrator.BuildRequest({"System", {message}, {}, "", std::nullopt, "s1"});
  if (result.ok()) {
    EXPECT_TRUE((*result)["input"].is_array());
  }
}



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

FUZZ_TEST(OrchestratorNormalizationFuzzTest, StoredResponsesItemNeverCrashes)
    .WithSeeds({std::string(R"({"type":"reasoning","encrypted_content":"opaque"})"),
                std::string(R"({"type":"message","role":"assistant","content":[]})"), std::string("not-json")});

}  // namespace
}  // namespace slop
