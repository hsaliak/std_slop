#include "core/tool_executor.h"
#include <gtest/gtest.h>
#include "absl/strings/match.h"
#include "core/database.h"

namespace slop {

TEST(ToolExecutorTest, LuaGlobalsInjectionCheck) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string session_id = "test_session";
  executor.SetSessionId(session_id);

  ASSERT_TRUE(db.UpdateScratchpad(session_id, "{\"test_key\": \"test_value\"}").ok());
  ASSERT_TRUE(db.SetSessionState(session_id, "working_on_tests").ok());
  ASSERT_TRUE(db.AppendMessage(session_id, "user", "Hello history").ok());

  std::string script = R"(
    if type(scratchpad) ~= "table" then error("scratchpad should be a table, got " .. type(scratchpad)) end
    if scratchpad.test_key ~= "test_value" then error("scratchpad content mismatch") end
    
    if type(state) ~= "string" then error("state should be a string, got " .. type(state)) end
    if state ~= "working_on_tests" then error("state mismatch") end
    
    if type(history) ~= "table" then error("history should be a table, got " .. type(history)) end
    if #history == 0 then error("history is empty") end
    
    return "globals_ok"
  )";

  auto res = executor.Execute("run_lua", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "Return Value: globals_ok"));
}

TEST(ToolExecutorTest, HelpToolCheck) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string script = "return tools.help()";
  auto res = executor.Execute("run_lua", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  
  EXPECT_TRUE(absl::StrContains(*res, "Slop Orchestrator Help")) << "Result: " << *res;
  EXPECT_TRUE(absl::StrContains(*res, "read_file({path, start_line, end_line, add_line_numbers=true})"));
  EXPECT_TRUE(absl::StrContains(*res, "git_grep_tool({pattern, patterns, path, context, case_insensitive, word_regexp, ...})"));
}

TEST(ToolExecutorTest, ScratchpadAutoConversionCheck) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string session_id = "test_session_raw";
  executor.SetSessionId(session_id);

  ASSERT_TRUE(db.UpdateScratchpad(session_id, "This is just notes").ok());

  std::string script = R"(
    if type(scratchpad) ~= "table" then error("scratchpad should be a table") end
    if scratchpad.notes ~= "This is just notes" then error("scratchpad notes mismatch") end
    return "raw_ok"
  )";

  auto res = executor.Execute("run_lua", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "Return Value: raw_ok"));
}

TEST(ToolExecutorTest, ManageScratchpadToolCheck) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string session_id = "test_session_tools";
  executor.SetSessionId(session_id);
  ASSERT_TRUE(db.Execute("INSERT INTO sessions (id, scratchpad) VALUES (?, ?)", {session_id, "{}"}).ok());

  std::string script = R"(
    tools.manage_scratchpad({action = "update", key = "foo", value = "bar"})
    return scratchpad.foo
  )";

  auto res = executor.Execute("run_lua", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "Return Value: bar"));
}

}  // namespace slop

