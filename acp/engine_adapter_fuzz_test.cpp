
#include "acp/engine_adapter.h"

#include <string>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop::acp {
namespace {

void ParseSessionPromptParamsNoCrash(const std::string& session_id, const std::string& prompt,
                                     bool include_session_id, bool include_prompt, bool params_object) {
  nlohmann::json params;
  if (params_object) {
    params = nlohmann::json::object();
    if (include_session_id) {
      params["sessionId"] = session_id;
    }
    if (include_prompt) {
      params["prompt"] = prompt;
    }
  } else {
    params = nlohmann::json::array();
    if (include_session_id) {
      params.push_back(session_id);
    }
    if (include_prompt) {
      params.push_back(prompt);
    }
  }

  auto parsed_or = ParseSessionPromptParams(params);
  if (parsed_or.ok()) {
    EXPECT_FALSE(parsed_or->session_id.empty());
    EXPECT_FALSE(parsed_or->prompt.empty());
  }
}

FUZZ_TEST(EngineAdapterFuzzTest, ParseSessionPromptParamsNoCrash)
    .WithDomains(fuzztest::Arbitrary<std::string>(),
                 fuzztest::Arbitrary<std::string>(),
                 fuzztest::Arbitrary<bool>(),
                 fuzztest::Arbitrary<bool>(),
                 fuzztest::Arbitrary<bool>());

}  // namespace
}  // namespace slop::acp