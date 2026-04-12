
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

void ParseSessionCancelParamsNoCrash(const std::string& session_id, bool include_session_id, bool params_object) {
  nlohmann::json params;
  if (params_object) {
    params = nlohmann::json::object();
    if (include_session_id) {
      params["sessionId"] = session_id;
    }
  } else {
    params = nlohmann::json::array();
    if (include_session_id) {
      params.push_back(session_id);
    }
  }

  auto parsed_or = ParseSessionCancelParams(params);
  if (parsed_or.ok()) {
    EXPECT_FALSE(parsed_or->session_id.empty());
  }
}

void ParseSessionCancelParamsDeterministic(const std::string& session_id, bool include_session_id, bool params_object) {
  nlohmann::json params = params_object ? nlohmann::json::object() : nlohmann::json::array();
  if (include_session_id) {
    if (params_object) params["sessionId"] = session_id;
    else params.push_back(session_id);
  }
  auto first = ParseSessionCancelParams(params);
  auto second = ParseSessionCancelParams(params);
  EXPECT_EQ(first.ok(), second.ok());
}

void ExecuteSessionPromptSlashCommandNoCrash(const std::string& prompt, bool include_leading_slash) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.Execute("INSERT INTO sessions (id) VALUES ('acp_1')").ok());

  SessionPromptRequest req;
  req.session_id = "acp_1";
  req.prompt = include_leading_slash ? ("/" + prompt) : prompt;

  bool executor_called = false;
  auto result_or = ExecuteSessionPrompt(
      &db, req,
      [&executor_called](const std::string&, const std::string&, std::shared_ptr<CancellationRequest>)
          -> absl::StatusOr<std::string> {
        executor_called = true;
        return std::string("ok");
      },
      nullptr);

  if (result_or.ok()) {
    EXPECT_TRUE(result_or->is_object());
    EXPECT_TRUE(result_or->contains("sessionId"));
    EXPECT_TRUE(result_or->contains("content") || result_or->contains("stopReason"));
  }

  if (include_leading_slash) {
    EXPECT_FALSE(executor_called);
  }
}

FUZZ_TEST(EngineAdapterFuzzTest, ParseSessionCancelParamsNoCrash)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<bool>(), fuzztest::Arbitrary<bool>());

FUZZ_TEST(EngineAdapterFuzzTest, ParseSessionCancelParamsDeterministic)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<bool>(), fuzztest::Arbitrary<bool>());

FUZZ_TEST(EngineAdapterFuzzTest, ExecuteSessionPromptSlashCommandNoCrash)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<bool>());

}  // namespace
}  // namespace slop::acp