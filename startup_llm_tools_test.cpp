#include "startup_llm_tools.h"

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <optional>

#include "core/database.h"
#include "core/json_utils.h"
#include "tools/tool_executor.h"

namespace slop {
namespace {

LlmToolSpecializationConfig MakeConfig(const std::string& tool_name, const std::string& system_prompt_patch,
                                       const std::string& session_id, const std::string& skill,
                                       std::optional<int> context_window = std::nullopt) {
  LlmToolSpecializationConfig cfg;
  cfg.tool_name = tool_name;
  cfg.system_prompt_patch = system_prompt_patch;
  cfg.session_id = session_id;
  cfg.skill = skill;
  cfg.context_window = context_window;
  return cfg;
}

TEST(StartupLlmToolsTest, RegistersConfiguredSpecializations) {
  const std::vector<LlmToolSpecializationConfig> configs = {
      MakeConfig("llm_tool.code_review_llm", "review code", "code_review", "code_reviewer", 8),
      MakeConfig("llm_tool.explorer_llm", "explore repository", "data_explorer", "data_explorer"),
  };

  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string captured_query;
  std::vector<std::string> captured_skills;
  LlmQueryOptions captured_options;
  auto status = RegisterLlmToolSpecializations(
      &db, &executor, configs, {"already_active"},
      [&](const std::string& query, const std::vector<std::string>& skills,
          const LlmQueryOptions& options) -> absl::StatusOr<std::string> {
        captured_query = query;
        captured_skills = skills;
        captured_options = options;
        return "ok";
      });
  ASSERT_TRUE(status.ok()) << status;

  auto tool_names = executor.GetRegisteredToolNamesForTest();
  EXPECT_NE(std::find(tool_names.begin(), tool_names.end(), "llm_tool.code_review_llm"), tool_names.end());
  EXPECT_NE(std::find(tool_names.begin(), tool_names.end(), "llm_tool.explorer_llm"), tool_names.end());

  auto tools_or = db.GetEnabledTools();
  ASSERT_TRUE(tools_or.ok()) << tools_or.status();
  auto has_tool = [&](const std::string& name) {
    return std::any_of(tools_or->begin(), tools_or->end(),
                       [&](const Database::Tool& t) { return t.name == name; });
  };
  EXPECT_TRUE(has_tool("llm_tool.code_review_llm"));
  EXPECT_TRUE(has_tool("llm_tool.explorer_llm"));

  auto args = json_parse(R"({"query":"review this"})");
  ASSERT_TRUE(args.has_value());
  auto exec_or = executor.Execute("llm_tool.code_review_llm", *args);
  ASSERT_TRUE(exec_or.ok()) << exec_or.status();
  EXPECT_EQ(*exec_or, "ok");
  EXPECT_EQ(captured_query, "review this");
  EXPECT_NE(std::find(captured_skills.begin(), captured_skills.end(), "already_active"), captured_skills.end());
  EXPECT_NE(std::find(captured_skills.begin(), captured_skills.end(), "code_reviewer"), captured_skills.end());
  EXPECT_EQ(captured_options.session_id, "code_review");
  ASSERT_TRUE(captured_options.skill.has_value());
  EXPECT_EQ(*captured_options.skill, "code_reviewer");
  ASSERT_TRUE(captured_options.context_window.has_value());
  EXPECT_EQ(*captured_options.context_window, 8);
}

TEST(StartupLlmToolsTest, EmptyConfigRegistersNothing) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto before = executor.GetRegisteredToolNamesForTest().size();
  auto status = RegisterLlmToolSpecializations(
      &db, &executor, {}, {},
      [](const std::string&, const std::vector<std::string>&,
         const LlmQueryOptions&) -> absl::StatusOr<std::string> { return "ok"; });
  ASSERT_TRUE(status.ok()) << status;

  auto after_names = executor.GetRegisteredToolNamesForTest();
  EXPECT_EQ(after_names.size(), before);
  EXPECT_EQ(std::count_if(after_names.begin(), after_names.end(), [](const std::string& n) {
              return n == "llm_tool.code_review_llm" || n == "llm_tool.explorer_llm";
            }),
            0);
}

TEST(StartupLlmToolsTest, RemovesStaleSpecializationToolsFromDatabase) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  ASSERT_TRUE(db.RegisterTool({"llm_tool.old", "old", "{}", true}).ok());
  ASSERT_TRUE(db.RegisterTool({"llm_tool.keep", "keep", "{}", true}).ok());
  ASSERT_TRUE(db.RegisterTool({"query_db", "builtin", "{}", true}).ok());

  ASSERT_TRUE(RegisterLlmToolSpecializations(&db, &executor, {MakeConfig("llm_tool.keep", "p", "s", "k")}, {},
                                             [](const std::string&, const std::vector<std::string>&, const LlmQueryOptions&)
                                                 -> absl::StatusOr<std::string> { return "ok"; })
                  .ok());

  auto tools_or = db.GetEnabledTools();
  ASSERT_TRUE(tools_or.ok()) << tools_or.status();
  auto has_tool = [&](const std::string& name) {
    return std::any_of(tools_or->begin(), tools_or->end(),
                       [&](const Database::Tool& t) { return t.name == name; });
  };

  EXPECT_FALSE(has_tool("llm_tool.old"));
  EXPECT_TRUE(has_tool("llm_tool.keep"));
  EXPECT_TRUE(has_tool("query_db"));
}

TEST(StartupLlmToolsTest, MissingQueryArgumentRejected) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  ASSERT_TRUE(RegisterLlmToolSpecializations(&db, &executor, {MakeConfig("llm_tool.x", "p", "s", "k")}, {},
                                             [](const std::string&, const std::vector<std::string>&, const LlmQueryOptions&)
                                                 -> absl::StatusOr<std::string> { return "ok"; })
                  .ok());
  auto args = json_parse("{}");
  ASSERT_TRUE(args.has_value());
  auto res = executor.Execute("llm_tool.x", *args);
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kInvalidArgument);
}
}  // namespace
}  // namespace slop
