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
    
    local found = false
    for _, msg in ipairs(history) do
      if msg.content == "Hello history" then found = true end
    end
    if not found then error("history content mismatch") end
    
    return "globals_ok"
  )";

  auto res = executor.Execute("run_lua", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "Return Value: globals_ok")) << "Actual result: " << *res;
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
    if scratchpad.notes ~= "This is just notes" then error("scratchpad notes mismatch: " .. tostring(scratchpad.notes)) end
    return "raw_ok"
  )";

  auto res = executor.Execute("run_lua", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "Return Value: raw_ok"));
}

TEST(ToolExecutorTest, EmptyGlobalsCheck) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string script = R"(
    if type(scratchpad) ~= "table" then error("scratchpad should be a table even if empty") end
    if type(history) ~= "table" then error("history should be a table even if empty") end
    return "empty_ok"
  )";

  auto res = executor.Execute("run_lua", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "Return Value: empty_ok"));
}

TEST(ToolExecutorTest, ManageScratchpadToolCheck) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string session_id = "test_session_tools";
  executor.SetSessionId(session_id);
  
  // Need to create the session in DB first because of FK constraints or logic
  ASSERT_TRUE(db.Execute("INSERT INTO sessions (id, scratchpad) VALUES (?, ?)", {session_id, "{}"}).ok());

  std::string script = R"(
    tools.manage_scratchpad({action = "update", key = "foo", value = "bar"})
    return scratchpad.foo
  )";

  auto res = executor.Execute("run_lua", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "Return Value: bar"));
  
  // Verify persistence in DB
  auto db_res = db.GetScratchpad(session_id);
  ASSERT_TRUE(db_res.ok());
  EXPECT_TRUE(absl::StrContains(*db_res, "\"foo\":\"bar\""));
}

}  // namespace slop

