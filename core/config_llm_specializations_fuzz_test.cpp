
#include "core/config.h"

#include <string>
#include <tuple>
#include <vector>

#include "absl/strings/match.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop {
namespace {

void ParseSpecializationsFromIniNeverCrashes(const std::string& ini_content) {
  const auto result = LoadLlmToolSpecializationsFromIni(ini_content);
  if (!result.ok()) {
    EXPECT_FALSE(result.ok());
    return;
  }

  for (const auto& cfg : *result) {
    EXPECT_TRUE(absl::StartsWith(cfg.tool_name, "llm_tool."));
    EXPECT_FALSE(cfg.tool_name.empty());
    EXPECT_FALSE(cfg.system_prompt_patch.empty());
    EXPECT_FALSE(cfg.session_id.empty());
    EXPECT_FALSE(cfg.skill.empty());
    if (cfg.context_window.has_value()) {
      EXPECT_GT(*cfg.context_window, 0);
    }
  }
}

void ParseSpecializationsDeterministic(const std::string& ini_content) {
  const auto first = LoadLlmToolSpecializationsFromIni(ini_content);
  const auto second = LoadLlmToolSpecializationsFromIni(ini_content);

  EXPECT_EQ(first.ok(), second.ok());
  if (!first.ok()) {
    EXPECT_EQ(first.status().code(), second.status().code());
    return;
  }

  ASSERT_EQ(first->size(), second->size());
  for (size_t i = 0; i < first->size(); ++i) {
    const auto& a = first->at(i);
    const auto& b = second->at(i);
    EXPECT_EQ(a.tool_name, b.tool_name);
    EXPECT_EQ(a.system_prompt_patch, b.system_prompt_patch);
    EXPECT_EQ(a.session_id, b.session_id);
    EXPECT_EQ(a.skill, b.skill);
    EXPECT_EQ(a.context_window, b.context_window);
  }
}

FUZZ_TEST(ConfigLlmSpecializationsFuzzTest, ParseSpecializationsFromIniNeverCrashes)
    .WithSeeds(std::vector<std::tuple<std::string>>{
        std::make_tuple(std::string(R"ini(
[llm_tool.code_review_llm]
system_prompt_patch = review code carefully
session_id = code_review
skill = code_reviewer
context_window = 20
)ini")),
        std::make_tuple(std::string(R"ini(
[llm_tool.]
system_prompt_patch = x
session_id = s
skill = k
)ini")),
        std::make_tuple(std::string(R"ini(
[llm_tool.code_review_llm]
system_prompt_patch =
session_id = code_review
skill = code_reviewer
)ini")),
    });

FUZZ_TEST(ConfigLlmSpecializationsFuzzTest, ParseSpecializationsDeterministic)
    .WithSeeds(std::vector<std::tuple<std::string>>{
        std::make_tuple(std::string("")),
        std::make_tuple(std::string("[slop]\nmodel = gemini-2.0-flash\n")),
        std::make_tuple(std::string("[llm_tool.explorer]\nsystem_prompt_patch = x\nsession_id = y\nskill = z\n")),
    });

}  // namespace
}  // namespace slop