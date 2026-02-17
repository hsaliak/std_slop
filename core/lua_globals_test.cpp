#include "absl/strings/match.h"

#include "core/database.h"
#include "core/tool_executor.h"

#include <gtest/gtest.h>

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
  EXPECT_TRUE(
      absl::StrContains(*res, "git_grep_tool({pattern, patterns, path, context, case_insensitive, word_regexp, ...})"));
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

TEST(ToolExecutorTest, LLMQueryStructuredTest) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string script = R"(
    -- Mock the underlying tool calls
    local captured = ""
    _G.call_tool = function(func, args) 
        captured = args.query
        return true, "mock_sync_success" 
    end
    _G.tools.dispatch_async = function(name, args)
        captured = args.query
        return "mock_async_job"
    end
    _G.native = { llm_query = function() end }

    -- 1. Positive Case: String
    local res = tools.llm_query("simple_string")
    if res ~= "mock_sync_success" or captured ~= "simple_string" then 
        error("Sync string fail: " .. tostring(res) .. ", " .. captured) 
    end

    -- 2. Positive Case: Table (query/context)
    tools.llm_query({query="q1", context="c1"})
    if not string.find(captured, "### INSTRUCTION ###\nq1") then error("Sync table fail 1: " .. captured) end
    if not string.find(captured, "### CONTEXT ###\nc1") then error("Sync table fail 2: " .. captured) end

    -- 3. Positive Case: Table (prompt/data aliases)
    tools.llm_query({prompt="p2", data="d2"})
    if not string.find(captured, "### INSTRUCTION ###\np2") then error("Sync table fail 3") end
    if not string.find(captured, "### CONTEXT ###\nd2") then error("Sync table fail 4") end

    -- 4. Positive Case: Array context
    tools.llm_query({query="q3", context={"d3a", "d3b"}})
    if not string.find(captured, "d3a\n\nd3b") then error("Sync table fail 5") end

    -- 5. Positive Case: Async
    local job = tools.llm_query_async({query="aq1", context="ac1"})
    if job ~= "mock_async_job" then error("Async fail: " .. tostring(job)) end
    if not string.find(captured, "### INSTRUCTION ###\naq1") then error("Async captured fail") end

    -- 6. Negative Case: Missing context
    local ok, err = pcall(tools.llm_query, {query="no_context"})
    if ok then error("Should have failed missing context") end
    if not string.find(err, "context MISSING") then error("Wrong error message: " .. err) end

    -- 7. Negative Case: Missing query
    ok, err = pcall(tools.llm_query, {context="no_query"})
    if ok then error("Should have failed missing query") end
    if not string.find(err, "query MISSING") then error("Wrong error message: " .. err) end

    -- 8. Negative Case: Invalid type
    ok, err = pcall(tools.llm_query, 123)
    if ok then error("Should have failed invalid type") end

    return "test_passed"
  )";

  auto res = executor.Execute("run_lua", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "Return Value: test_passed")) << "Output: " << *res;
}

TEST(ToolExecutorTest, HelpUpdateCheck) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string script = "return tools.help()";
  auto res = executor.Execute("run_lua", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();

  EXPECT_TRUE(absl::StrContains(*res, "llm_query({query, context}): (string) Runs a sub-task LLM query."))
      << "Help content missing expected llm_query update";
  EXPECT_TRUE(absl::StrContains(
      *res, "Accepts string or table: { query = \"instruction\", context = \"data\" | {\"data1\", \"data2\"} }"))
      << "Help content missing expected table format details";
}

}  // namespace slop
