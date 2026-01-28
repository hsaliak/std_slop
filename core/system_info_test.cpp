#include "core/database.h"
#include "core/orchestrator.h"

#include "absl/strings/match.h"

#include <gtest/gtest.h>
namespace slop {

class SystemInfoTest : public ::testing::Test {
 protected:
  Database db;
  HttpClient http;

  void SetUp() override { ASSERT_TRUE(db.Init(":memory:").ok()); }
};

TEST_F(SystemInfoTest, BuiltinPromptIsLoaded) {
  auto orchestrator_or = Orchestrator::Builder(&db, &http).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto& orchestrator = *orchestrator_or;

  // Create a dummy session
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());

  auto result = orchestrator->AssemblePrompt("s1", {});
  ASSERT_TRUE(result.ok());

  nlohmann::json prompt = *result;
  ASSERT_TRUE(prompt.contains("system_instruction"));
  std::string instr = prompt["system_instruction"]["parts"][0]["text"];

  // Check for core content from system_prompt.md
  // Since kBuiltinSystemPrompt is baked in at compile time from system_prompt.md,
  // we check for high-level strings we know are there.
  EXPECT_TRUE(absl::StrContains(instr, "interactive CLI agent")) << "Missing character definition";
  EXPECT_TRUE(absl::StrContains(instr, "Software Engineering Tasks")) << "Missing workflow definition";
  EXPECT_TRUE(absl::StrContains(instr, "Context Retrieval")) << "Missing context retrieval instruction";
  EXPECT_TRUE(absl::StrContains(instr, "---AVAILABLE TOOLS---")) << "Missing tools section header";
}

}  // namespace slop
