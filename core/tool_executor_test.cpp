#include "tools/tool_executor.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

#include "tools/tool_dispatcher.h"

#include <gtest/gtest.h>

namespace slop {
namespace {

nlohmann::json ParseEnvelope(const std::string& raw) {
  auto parsed = nlohmann::json::parse(raw, nullptr, false);
  if (parsed.is_discarded()) {
    return {};
  }
  return parsed;
}

std::string EnvelopeResultText(const std::string& raw) {
  const auto env = ParseEnvelope(raw);
  if (!env.is_object()) {
    return raw;
  }
  if (env.contains("result")) {
    if (env["result"].is_string()) {
      return env["result"].get<std::string>();
    }
    return env["result"].dump();
  }
  if (env.contains("error")) {
    if (env["error"].is_object() && env["error"].contains("message") && env["error"]["message"].is_string()) {
      return env["error"]["message"].get<std::string>();
    }
    if (env["error"].is_string()) {
      return env["error"].get<std::string>();
    }
    return env["error"].dump();
  }
  return raw;
}

}  // namespace

TEST(ToolExecutorTest, ReadWriteFile) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string test_file = "test_executor.txt";
  std::string content = "Hello from ToolExecutor";

  auto write_res = executor.Execute("write_file", {{"path", test_file}, {"content", content}});
  ASSERT_TRUE(write_res.ok());
  EXPECT_TRUE(absl::StrContains(*write_res, "File written successfully:"));
  EXPECT_TRUE(absl::StrContains(*write_res, "Path: test_executor.txt"));

  auto read_res = executor.Execute("read_file", {{"path", test_file}});
  ASSERT_TRUE(read_res.ok());
  const std::string read_text = *read_res;
  EXPECT_TRUE(read_text.find(content) != std::string::npos);
  EXPECT_TRUE(!absl::StrContains(read_text, "1: "));

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, ReadFileGranular) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string test_file = "test_granular.txt";
  std::string content = "Line 1\nLine 2\nLine 3\nLine 4\nLine 5\n";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", content}}).ok());

  // Test: Specific range
  auto res1 = executor.Execute("read_file", {{"path", test_file}, {"start_line", 2}, {"end_line", 4}});
  ASSERT_TRUE(res1.ok());
  const std::string text1 = EnvelopeResultText(*res1);
  EXPECT_TRUE(absl::StrContains(text1, "Line 2\nLine 3\nLine 4\n"));
  EXPECT_TRUE(!absl::StrContains(text1, "1: Line 1"));
  EXPECT_TRUE(!absl::StrContains(text1, "5: Line 5"));

  // Test: Start only
  auto res2 = executor.Execute("read_file", {{"path", test_file}, {"start_line", 4}});
  ASSERT_TRUE(res2.ok());
  const std::string text2 = EnvelopeResultText(*res2);
  EXPECT_TRUE(absl::StrContains(text2, "Line 4\nLine 5\n"));
  EXPECT_TRUE(!absl::StrContains(text2, "3: Line 3"));

  // Test: End only
  auto res3 = executor.Execute("read_file", {{"path", test_file}, {"end_line", 2}});
  ASSERT_TRUE(res3.ok()) << res3.status().message();
  const std::string text3 = EnvelopeResultText(*res3);
  EXPECT_TRUE(absl::StrContains(text3, "Line 1\nLine 2\n"));
  EXPECT_TRUE(!absl::StrContains(text3, "3: Line 3"));

  // Test: Out of bounds
  auto res4 = executor.Execute("read_file", {{"path", test_file}, {"start_line", 10}});
  ASSERT_FALSE(res4.ok());
  EXPECT_EQ(res4.status().code(), absl::StatusCode::kInvalidArgument);

  // Test: Invalid range
  auto res5 = executor.Execute("read_file", {{"path", test_file}, {"start_line", 5}, {"end_line", 2}});
  ASSERT_FALSE(res5.ok());
  EXPECT_EQ(res5.status().code(), absl::StatusCode::kInvalidArgument);

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, ReadFileMetadata) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string test_file = "test_metadata.txt";
  std::string content = "Line 1\nLine 2\nLine 3\n";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", content}}).ok());

  auto res = executor.Execute("read_file", {{"path", test_file}, {"start_line", 1}, {"end_line", 2}});
  ASSERT_TRUE(res.ok());

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, MailModelEnforcement) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;
  executor.SetMailMode(true);

  // 1. Test failure on 'main' branch
  setenv("SLOP_FORCE_BRANCH_NAME", "main", 1);
  unsetenv("SLOP_SKIP_STAGING_CHECK");

  auto write_res = executor.Execute("write_file", {{"path", "fail_test.txt"}, {"content", "fail"}});
  EXPECT_FALSE(write_res.ok());
  EXPECT_TRUE(absl::StrContains(write_res.status().message(), "Destructive operations are only allowed"));

  // 2. Test success on staging branch
  setenv("SLOP_FORCE_BRANCH_NAME", "slop/staging/test-feature", 1);
  auto write_res2 = executor.Execute("write_file", {{"path", "success_test.txt"}, {"content", "success"}});
  EXPECT_TRUE(write_res2.ok());

  // 3. Test that read_file (non-protected) works on any branch
  // Using success_test.txt which we just wrote, so we know it exists in the sandbox.
  setenv("SLOP_FORCE_BRANCH_NAME", "main", 1);
  auto read_res = executor.Execute("read_file", {{"path", "success_test.txt"}, {"start_line", 1}, {"end_line", 1}});
  EXPECT_TRUE(read_res.ok());

  std::filesystem::remove("success_test.txt");

  // Clean up environment
  unsetenv("SLOP_FORCE_BRANCH_NAME");
  setenv("SLOP_SKIP_STAGING_CHECK", "1", 1);  // Restore for other tests if they run in same process
}

TEST(ToolExecutorTest, MailModelEnforcementExactDenialString) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  executor.SetMailMode(true);
  setenv("SLOP_FORCE_BRANCH_NAME", "main", 1);
  unsetenv("SLOP_SKIP_STAGING_CHECK");

  auto res = executor.Execute("write_file", {{"path", "deny_exact.txt"}, {"content", "x"}});
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_TRUE(
      absl::StrContains(res.status().message(),
                        "Destructive operations are only allowed on 'slop/staging/*' branches. Current branch: main"))
      << res.status().message();

  unsetenv("SLOP_FORCE_BRANCH_NAME");
  setenv("SLOP_SKIP_STAGING_CHECK", "1", 1);
}

TEST(ToolExecutorTest, MailModelEnforcementAppliesToAllMailTools) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;
  executor.SetMailMode(true);

  setenv("SLOP_FORCE_BRANCH_NAME", "main", 1);
  unsetenv("SLOP_SKIP_STAGING_CHECK");

  const std::vector<std::pair<std::string, nlohmann::json>> mail_tools = {
      {"write_file", {{"path", "mm_guard.txt"}, {"content", "x"}}},
      {"edit_tool", {{"path", "mm_guard.txt"},
                     {"edits", nlohmann::json::array({{{"op", "replace"}, {"find", "x"}, {"text", "y"}}})}}},
      {"execute_bash", {{"command", "echo guard"}}},
      {"git_commit_patch", {{"summary", "s"}, {"rationale", "r"}}},
      {"git_format_patch_series", nlohmann::json::object()},
      {"git_finalize_series", nlohmann::json::object()},
      {"git_reroll_patch", {{"index", 1}}},
  };

  for (const auto& [name, args] : mail_tools) {
    auto res = executor.Execute(name, args);
    ASSERT_FALSE(res.ok()) << "expected slop-guard denial for: " << name;
    EXPECT_EQ(res.status().code(), absl::StatusCode::kFailedPrecondition) << name;
    EXPECT_TRUE(
        absl::StrContains(res.status().message(),
                          "Destructive operations are only allowed on 'slop/staging/*' branches. Current branch: main"))
        << name << " -> " << res.status().message();
  }

  unsetenv("SLOP_FORCE_BRANCH_NAME");
  setenv("SLOP_SKIP_STAGING_CHECK", "1", 1);
}

TEST(ToolExecutorTest, StandardModeBypassesStagingGuardForMailTools) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  // Standard mode should bypass staging-branch restriction.
  executor.SetMailMode(false);
  setenv("SLOP_FORCE_BRANCH_NAME", "main", 1);
  unsetenv("SLOP_SKIP_STAGING_CHECK");

  auto res = executor.Execute("write_file", {{"path", "standard_mode_ok.txt"}, {"content", "ok"}});
  ASSERT_TRUE(res.ok()) << res.status().message();

  std::filesystem::remove("standard_mode_ok.txt");
  unsetenv("SLOP_FORCE_BRANCH_NAME");
  setenv("SLOP_SKIP_STAGING_CHECK", "1", 1);
}

TEST(ToolExecutorTest, ExecuteBash) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("execute_bash", {{"command", "echo 'slop'"}});
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(absl::StrContains(*res, "\"stdout\":\"slop\\n\""));
  EXPECT_TRUE(absl::StrContains(*res, "\"stderr\":\"\""));
  EXPECT_TRUE(absl::StrContains(*res, "\"exit_code\":0"));
  EXPECT_TRUE(absl::StrContains(*res, "\"timeout_seconds\":180"));
}

TEST(ToolExecutorTest, ExecuteBashRejectsNegativeTimeout) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("execute_bash", {{"command", "echo hi"}, {"timeout_seconds", -1}});
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(res.status().message(), "timeout_seconds must be >= 0"));
}

TEST(ToolExecutorTest, ExecuteBashTimeoutReturnsStructuredError) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("execute_bash", {{"command", "sleep 2"}, {"timeout_seconds", 1}});
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(absl::StrContains(*res, "Error: {"));
  EXPECT_TRUE(absl::StrContains(*res, "\"status\":\"DEADLINE_EXCEEDED\""));
  EXPECT_TRUE(absl::StrContains(*res, "\"error\":\"Command timed out\""));
  EXPECT_TRUE(absl::StrContains(*res, "\"timeout_seconds\":1"));
}

TEST(ToolExecutorTest, ToolNotFound) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("non_existent", {});
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kNotFound);
  EXPECT_TRUE(absl::StrContains(res.status().message(), "NOT_FOUND: Tool not found: non_existent"));
}

TEST(ToolExecutorTest, DroppedToolsAreNotFoundTopLevel) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  for (const std::string& dropped : {"execute_bash_async", "help", "llm_query_async", "shell_escape"}) {
    auto res = executor.Execute(dropped, nlohmann::json::object());
    ASSERT_FALSE(res.ok()) << "expected NOT_FOUND for dropped tool: " << dropped;
    EXPECT_EQ(res.status().code(), absl::StatusCode::kNotFound);
    EXPECT_TRUE(absl::StrContains(res.status().message(), absl::StrCat("NOT_FOUND: Tool not found: ", dropped)))
        << res.status().message();
  }
}

TEST(ToolExecutorTest, PromotedCoreToolsAreRegisteredTopLevel) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  const std::vector<std::string> core_tools = {
      "describe_db", "git_create_staging_branch", "grep", "list_directory", "read_file", "use_skill"};

  for (const auto& name : core_tools) {
    auto res = executor.Execute(name, nlohmann::json::object());
    EXPECT_NE(res.status().code(), absl::StatusCode::kNotFound) << "expected registered top-level tool: " << name;
  }
}

TEST(ToolExecutorTest, PromotedMailToolsAreRegisteredTopLevel) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  // Keep standard mode so registration checks are not blocked by staging-branch policy.
  executor.SetMailMode(false);

  const std::vector<std::string> mail_tools = {
      "execute_bash",     "git_commit_patch", "git_finalize_series", "git_format_patch_series",
      "git_reroll_patch", "edit_tool",        "write_file",
  };

  for (const auto& name : mail_tools) {
    auto res = executor.Execute(name, nlohmann::json::object());
    EXPECT_NE(res.status().code(), absl::StatusCode::kNotFound) << "expected registered top-level tool: " << name;
  }
}

TEST(ToolExecutorTest, RegisteredToolSurfaceMatchesLockedCanonicalList) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  const std::vector<std::string> expected = {
      "ask_user",
      "describe_db",
      "edit_tool",
      "execute_bash",
      "git_commit_patch",
      "git_create_staging_branch",
      "git_finalize_series",
      "git_format_patch_series",
      "git_reroll_patch",
      "git_verify_series",
      "grep",
      "list_directory",
      "patch_tool",
      "persist_function",
      "query_db",
      "read_file",
      "read_scratchpad",
      "run_js",
      "use_skill",
      "write_file",
      "write_scratchpad",
  };

  EXPECT_EQ(executor.GetRegisteredToolNamesForTest(), expected);

  for (const std::string& dropped : {"execute_bash_async", "help", "llm_query_async", "shell_escape"}) {
    auto res = executor.Execute(dropped, nlohmann::json::object());
    ASSERT_FALSE(res.ok()) << "expected NOT_FOUND for dropped tool: " << dropped;
    EXPECT_EQ(res.status().code(), absl::StatusCode::kNotFound);
  }
}

TEST(ToolExecutorTest, RunJsCanPersistJavaScriptFunction) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  const std::string code = R"js(
const persisted = tools.persist_function({
  name: 'doubleValue',
  code: 'function doubleValue(value) { return value * 2; }',
  description: 'Double a number',
  json_schema: '{"type":"number"}',
  test_args: [21]
});
const rows = tools.dispatch('query_db', {
  sql: 'SELECT name, description, json_schema FROM js_functions WHERE name = ?',
  params: ['doubleValue']
});
return { persisted, rows };
)js";
  auto persist_res = executor.Execute("run_js", {{"code", code}});
  ASSERT_TRUE(persist_res.ok()) << persist_res.status();
  const nlohmann::json persist_result = ParseEnvelope(*persist_res);
  ASSERT_EQ(persist_result["persisted"]["persisted"], true);
  ASSERT_EQ(persist_result["persisted"]["name"], "doubleValue");
  ASSERT_EQ(persist_result["rows"].size(), 1);
  EXPECT_EQ(persist_result["rows"][0]["description"], "Double a number");

  auto invoke_res = executor.Execute("run_js", {{"code", "return doubleValue(21);"}});
  ASSERT_TRUE(invoke_res.ok()) << invoke_res.status();
  EXPECT_EQ(ParseEnvelope(*invoke_res), 42);

  auto help_res = executor.Execute("run_js", {{"code", "return tools.help().persisted_globals;"}});
  ASSERT_TRUE(help_res.ok()) << help_res.status();
  const nlohmann::json persisted_globals = ParseEnvelope(*help_res);
  ASSERT_EQ(persisted_globals.size(), 1);
  EXPECT_EQ(persisted_globals[0]["name"], "doubleValue");
  EXPECT_EQ(persisted_globals[0]["description"], "Double a number");
}

TEST(ToolExecutorTest, PersistFunctionRejectsInvalidShapeBeforeMutation) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  struct Case {
    std::string name;
    std::string code;
    std::string expected_error;
  };
  const std::vector<Case> cases = {
      {"missingCode", "return tools.persist_function({ name: 'badHelper' });",
       "persist_function requires string field code"},
      {"invalidName", "return tools.persist_function({ name: 'bad-helper', code: 'function bad() { return 1; }' });",
       "persist_function name must be a valid JavaScript identifier"},
      {"reservedName", "return tools.persist_function({ name: 'tools', code: 'function tools() { return 1; }' });",
       "persist_function name collides with a built-in run_js global"},
      {"intrinsicName", "return tools.persist_function({ name: 'Function', code: 'function Function() { return 1; }' });",
       "persist_function name collides with a built-in run_js global"},
      {"globalName", "return tools.persist_function({ name: 'Map', code: 'function Map() { return 1; }' });",
       "persisted function name collides with existing run_js global"},
      {"invalidSchema", "return tools.persist_function({ name: 'badSchema', code: 'function badSchema() { return 1; }', json_schema: 'not json' });",
       "persist_function json_schema must be a JSON object string"},
      {"syntaxError", "return tools.persist_function({ name: 'syntaxBad', code: 'function syntaxBad( {' });",
       "persist_function code must be a single named function declaration matching name"},
      {"wrongName", "return tools.persist_function({ name: 'wrongName', code: 'function otherName() { return 1; }' });",
       "persist_function code must be a single named function declaration matching name"},
      {"topLevelSideEffect", "return tools.persist_function({ name: 'sideEffect', code: 'function sideEffect() { return 1; }\\ntools.dispatch(\\\"query_db\\\", {sql: \\\"SELECT 1\\\"});' });",
       "persist_function code must be a single named function declaration matching name"},
      {"oversizeCode", "return tools.persist_function({ name: 'tooLarge', code: 'function tooLarge() { return \\\"' + 'x'.repeat(70 * 1024) + '\\\"; }' });",
       "persist_function code exceeds maximum size"},
      {"failingTestArg", "return tools.persist_function({ name: 'mustBePositive', code: 'function mustBePositive(value) { if (value <= 0) throw new Error(\\\"bad arg\\\"); return value; }', test_args: [0] });",
       "bad arg"},
  };

  for (const auto& test_case : cases) {
    auto res = executor.Execute("run_js", {{"code", test_case.code}});
    ASSERT_FALSE(res.ok()) << test_case.name;
    EXPECT_TRUE(absl::StrContains(res.status().message(), test_case.expected_error))
        << test_case.name << ": " << res.status().message();
  }

  auto rows = db.Query("SELECT name FROM js_functions WHERE name LIKE '%bad%' OR name IN ('wrongName', 'tools', 'mustBePositive', 'sideEffect', 'tooLarge') ORDER BY name");
  ASSERT_TRUE(rows.ok()) << rows.status();
  EXPECT_EQ(*rows, "[]");

  auto still_usable = executor.Execute("run_js", {{"code", "return 7;"}});
  ASSERT_TRUE(still_usable.ok()) << still_usable.status();
  EXPECT_EQ(ParseEnvelope(*still_usable), 7);
}

TEST(ToolExecutorTest, PersistFunctionRejectsAggregatePreloadLimitBeforeMutation) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  const std::string large_literal(50 * 1024, 'x');
  const std::string existing_code_a = absl::StrCat("function existingA() { return '", large_literal, "'; }");
  const std::string existing_code_b = absl::StrCat("function existingB() { return '", large_literal, "'; }");
  ASSERT_TRUE(db.Execute(
                  "INSERT INTO js_functions(name, code, description, json_schema) VALUES (?, ?, '', '')",
                  "existingA", existing_code_a)
                  .ok());
  ASSERT_TRUE(db.Execute(
                  "INSERT INTO js_functions(name, code, description, json_schema) VALUES (?, ?, '', '')",
                  "existingB", existing_code_b)
                  .ok());

  const std::string candidate_code = absl::StrCat(
      "return tools.persist_function({ name: 'candidateLarge', code: 'function candidateLarge() { return \\\"",
      large_literal, "\\\"; }' });");
  auto res = executor.Execute("run_js", {{"code", candidate_code}});
  ASSERT_FALSE(res.ok());
  EXPECT_TRUE(absl::StrContains(res.status().message(), "persist_function would exceed persisted preload size limit"))
      << res.status().message();

  auto rows = db.Query("SELECT name FROM js_functions WHERE name = ?", {"candidateLarge"});
  ASSERT_TRUE(rows.ok()) << rows.status();
  EXPECT_EQ(*rows, "[]");
}

TEST(ToolExecutorTest, SlopGuardIsInternalOnlyAndNotTopLevelCallable) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  // Guard helper is an internal C++ function and must not be callable as a
  // top-level tool.
  auto top_level = executor.Execute("slop_guard", nlohmann::json::object());
  ASSERT_FALSE(top_level.ok());
  EXPECT_EQ(top_level.status().code(), absl::StatusCode::kNotFound);
}

TEST(ToolExecutorTest, DescribeDbTopLevelReturnsSchemaRows) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("describe_db", nlohmann::json::object());
  ASSERT_TRUE(res.ok()) << res.status().message();
  // Stable parity anchor: sqlite schema includes sessions table in initialized DB.
  EXPECT_TRUE(absl::StrContains(*res, "sessions")) << *res;
}

TEST(ToolExecutorTest, UseSkillTopLevelRequiresActiveSession) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("use_skill", {{"name", "test_skill"}, {"action", "activate"}});
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_TRUE(absl::StrContains(res.status().message(), "No active session")) << res.status().message();
}

TEST(ToolExecutorTest, GrepTopLevelRequiresPattern) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("grep", nlohmann::json::object());
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(res.status().message(), "Missing mandatory field: pattern"));
}

TEST(ToolExecutorTest, GrepTopLevelFixedStringsAndTruncation) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  const std::string test_file = "grep_fixed_truncate.txt";
  std::string content;
  for (int i = 0; i < 20; ++i) {
    content += "a+b\n";
  }
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", content}}).ok());

  auto regex_res = executor.Execute("grep", {{"pattern", "a+b"}, {"path", test_file}});
  ASSERT_TRUE(regex_res.ok());
  EXPECT_TRUE(regex_res->empty()) << *regex_res;

  auto fixed_res =
      executor.Execute("grep", {{"pattern", "a+b"}, {"path", test_file}, {"fixed_strings", true}, {"limit", 5}});
  ASSERT_TRUE(fixed_res.ok()) << fixed_res.status().message();
  EXPECT_TRUE(absl::StrContains(*fixed_res, "a+b"));
  EXPECT_TRUE(absl::StrContains(*fixed_res, "[TRUNCATED: Use a more specific pattern or path to narrow results]"));

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, GrepTopLevelIgnoreArrayRespected) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  ASSERT_TRUE(
      executor
          .Execute("execute_bash",
                   {{"command",
                     "mkdir -p grep_ignore_dir/ignoredir && printf 'needle\n' > grep_ignore_dir/ignoredir/file.txt"}})
          .ok());

  auto include_res = executor.Execute("grep", {{"pattern", "needle"}, {"path", "grep_ignore_dir"}});
  ASSERT_TRUE(include_res.ok());
  EXPECT_TRUE(absl::StrContains(*include_res, "needle"));

  auto ignore_res = executor.Execute(
      "grep", {{"pattern", "needle"}, {"path", "grep_ignore_dir"}, {"ignore", nlohmann::json::array({"ignoredir"})}});
  ASSERT_TRUE(ignore_res.ok());
  EXPECT_TRUE(ignore_res->empty()) << *ignore_res;

  (void)executor.Execute("execute_bash", {{"command", "rm -rf grep_ignore_dir"}});
}

TEST(ToolExecutorTest, GitCreateStagingBranchRequiresName) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("git_create_staging_branch", nlohmann::json::object());
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(res.status().message(), "name is required"));
}

TEST(ToolExecutorTest, AliasToolNameResolvesToCanonical) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("list_dir", {{"path", "."}, {"depth", 1}});
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kNotFound);
  EXPECT_TRUE(absl::StrContains(res.status().message(), "NOT_FOUND: Tool not found: list_dir"));
}

TEST(ToolExecutorTest, QueryDb) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("query_db", {{"sql", "SELECT 1 as val"}});
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(res->find("\"val\":1") != std::string::npos);
}

TEST(ToolExecutorTest, ExecuteBashFailure) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("execute_bash", {{"command", "exit 42"}});
  ASSERT_TRUE(res.ok());
  // Execute wraps the error in a string for the LLM
  EXPECT_TRUE(res->find("Error: INTERNAL: Command failed with status 42") != std::string::npos);
}

TEST(ToolExecutorTest, ExecuteBashStderr) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("execute_bash", {{"command", "echo 'hello stdout' && echo 'hello stderr' >&2"}});
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(res->find("hello stdout") != std::string::npos);
  EXPECT_TRUE(res->find("### STDERR") != std::string::npos);
  EXPECT_TRUE(res->find("hello stderr") != std::string::npos);
}

TEST(ToolExecutorTest, EditToolAppliesSequentialEdits) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  const std::string test_file = "edit_success.txt";
  const std::string initial_content =
      "alpha\n"
      "beta\n"
      "gamma\n"
      "beta\n";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", initial_content}}).ok());

  nlohmann::json edits = nlohmann::json::array(
      {{{"op", "replace"}, {"find", "beta"}, {"text", "BETA"}, {"which", "first"}},
       {{"op", "insert_after"}, {"find", "gamma"}, {"text", "\ndelta"}},
       {{"op", "insert_before"}, {"find", "alpha"}, {"text", "start\n"}},
       {{"op", "delete"}, {"find", "beta"}, {"which", "last"}}});

  auto edit_res = executor.Execute("edit_tool", {{"path", test_file}, {"edits", edits}});
  ASSERT_TRUE(edit_res.ok()) << edit_res.status().message();
  const std::string edit_text = EnvelopeResultText(*edit_res);
  EXPECT_TRUE(absl::StrContains(edit_text, R"("edits":4)"));
  EXPECT_TRUE(absl::StrContains(edit_text, R"("changes")"));

  auto read_res = executor.Execute("read_file", {{"path", test_file}});
  ASSERT_TRUE(read_res.ok());
  const std::string content = EnvelopeResultText(*read_res);
  EXPECT_TRUE(absl::StrContains(content, "start\nalpha\nBETA\ngamma\ndelta\n"));
  EXPECT_TRUE(!absl::StrContains(content, "delta\nbeta"));

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, EditToolOnlyFailsOnAmbiguousMatchAndLeavesFileUnchanged) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  const std::string test_file = "edit_ambiguous.txt";
  const std::string initial_content = "same\nsame\n";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", initial_content}}).ok());

  nlohmann::json edits = nlohmann::json::array({{{"op", "replace"}, {"find", "same"}, {"text", "other"}}});
  auto edit_res = executor.Execute("edit_tool", {{"path", test_file}, {"edits", edits}});
  ASSERT_FALSE(edit_res.ok());
  EXPECT_TRUE(absl::StrContains(std::string(edit_res.status().message()), "expected exactly one match"));

  auto read_res = executor.Execute("read_file", {{"path", test_file}});
  ASSERT_TRUE(read_res.ok());
  EXPECT_TRUE(absl::StrContains(EnvelopeResultText(*read_res), "same\nsame\n"));

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, EditToolFailureAfterEarlierEditLeavesFileUnchanged) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  const std::string test_file = "edit_atomic.txt";
  const std::string initial_content = "alpha\nbeta\n";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", initial_content}}).ok());

  nlohmann::json edits = nlohmann::json::array({{{"op", "replace"}, {"find", "alpha"}, {"text", "ALPHA"}},
                                                {{"op", "delete"}, {"find", "missing"}}});
  auto edit_res = executor.Execute("edit_tool", {{"path", test_file}, {"edits", edits}});
  ASSERT_FALSE(edit_res.ok());
  EXPECT_TRUE(absl::StrContains(std::string(edit_res.status().message()), "find text was not found"));

  auto read_res = executor.Execute("read_file", {{"path", test_file}});
  ASSERT_TRUE(read_res.ok());
  EXPECT_TRUE(absl::StrContains(EnvelopeResultText(*read_res), "alpha\nbeta\n"));
  EXPECT_TRUE(!absl::StrContains(EnvelopeResultText(*read_res), "ALPHA"));

  std::filesystem::remove(test_file);
}


TEST(ToolExecutorTest, EditToolRejectsInvalidArgShapesWithoutChangingFile) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  const std::string test_file = "edit_invalid_args.txt";
  const std::string initial_content = "alpha\nbeta\n";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", initial_content}}).ok());

  const std::vector<nlohmann::json> invalid_args = {
      nlohmann::json::array(),
      {{"edits", nlohmann::json::array()}},
      {{"path", test_file}, {"edits", "not-array"}},
      {{"path", test_file}, {"edits", nlohmann::json::array()}},
      {{"path", test_file}, {"edits", nlohmann::json::array({"not-object"})}},
      {{"path", test_file}, {"edits", nlohmann::json::array({{{"find", "alpha"}, {"text", "ALPHA"}}})}},
      {{"path", test_file}, {"edits", nlohmann::json::array({{{"op", "replace"}, {"find", ""}, {"text", "ALPHA"}}})}},
      {{"path", test_file}, {"edits", nlohmann::json::array({{{"op", "replace"}, {"find", "alpha"}}})}},
      {{"path", test_file}, {"edits", nlohmann::json::array({{{"op", "replace"}, {"find", "alpha"}, {"text", "alpha"}}})}},
      {{"path", test_file}, {"edits", nlohmann::json::array({{{"op", "delete"}, {"find", "alpha"}, {"text", "ignored"}}})}},
      {{"path", test_file}, {"edits", nlohmann::json::array({{{"op", "replace"}, {"find", "alpha"}, {"text", "ALPHA"}, {"which", "bad"}}})}},
      {{"path", test_file}, {"edits", nlohmann::json::array({{{"op", "replace"}, {"find", "alpha"}, {"text", "ALPHA"}, {"which", -1}}})}},
      {{"path", test_file}, {"edits", nlohmann::json::array({{{"op", "replace"}, {"find", "alpha"}, {"text", "ALPHA"}, {"which", true}}})}},
      {{"path", test_file}, {"edits", nlohmann::json::array({{{"op", "replace"}, {"find", "alpha"}, {"text", "ALPHA"}, {"which", 2}}})}},
  };

  for (const auto& args : invalid_args) {
    auto edit_res = executor.Execute("edit_tool", args);
    EXPECT_FALSE(edit_res.ok()) << args.dump();

    auto read_res = executor.Execute("read_file", {{"path", test_file}});
    ASSERT_TRUE(read_res.ok());
    EXPECT_TRUE(absl::StrContains(EnvelopeResultText(*read_res), initial_content)) << args.dump();
  }

  nlohmann::json valid_edits =
      nlohmann::json::array({{{"op", "replace"}, {"find", "alpha"}, {"text", "ALPHA"}}});
  EXPECT_FALSE(executor.Execute("edit_tool", {{"path", "/tmp/edit_invalid_args.txt"}, {"edits", valid_edits}}).ok());
  EXPECT_FALSE(executor.Execute("edit_tool", {{"path", "../edit_invalid_args.txt"}, {"edits", valid_edits}}).ok());

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, EditToolFirstLastAndOnlySelectors) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  const std::string test_file = "edit_selectors.txt";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", "x x x"}}).ok());

  nlohmann::json edits = nlohmann::json::array({{{"op", "replace"}, {"find", "x"}, {"text", "a"}, {"which", "first"}},
                                                {{"op", "replace"}, {"find", "x"}, {"text", "z"}, {"which", "last"}},
                                                {{"op", "replace"}, {"find", "a"}, {"text", "A"}, {"which", "only"}}});
  auto edit_res = executor.Execute("edit_tool", {{"path", test_file}, {"edits", edits}});
  ASSERT_TRUE(edit_res.ok()) << edit_res.status().message();

  auto read_res = executor.Execute("read_file", {{"path", test_file}});
  ASSERT_TRUE(read_res.ok());
  EXPECT_TRUE(absl::StrContains(EnvelopeResultText(*read_res), "A x z"));

  std::filesystem::remove(test_file);
}
TEST(ToolExecutorTest, EditToolNumericWhichSelectsOccurrence) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  const std::string test_file = "edit_numeric.txt";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", "x x x"}}).ok());

  nlohmann::json edits = nlohmann::json::array({{{"op", "replace"}, {"find", "x"}, {"text", "y"}, {"which", 1}}});
  auto edit_res = executor.Execute("edit_tool", {{"path", test_file}, {"edits", edits}});
  ASSERT_TRUE(edit_res.ok()) << edit_res.status().message();

  auto read_res = executor.Execute("read_file", {{"path", test_file}});
  ASSERT_TRUE(read_res.ok());
  EXPECT_TRUE(absl::StrContains(EnvelopeResultText(*read_res), "x y x"));

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, AskUser) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  bool handler_called = false;
  std::string received_prompt;

  executor.SetAskUserHandler([&](const std::string& p) {
    handler_called = true;
    received_prompt = p;
    return "User typed this";
  });

  auto res = executor.Execute("ask_user", {{"prompt", "Are you sure?"}});
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(handler_called);
  EXPECT_EQ(received_prompt, "Are you sure?");
  EXPECT_TRUE(res->find("User typed this") != std::string::npos);
}

TEST(ToolExecutorTest, ScratchpadToolsReadWrite) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;
  executor.SetSessionId("s1");

  auto write_res = executor.Execute("write_scratchpad", {{"content", "plan step 1"}});
  ASSERT_TRUE(write_res.ok()) << write_res.status().message();

  auto read_res = executor.Execute("read_scratchpad", nlohmann::json::object());
  ASSERT_TRUE(read_res.ok()) << read_res.status().message();
  EXPECT_EQ(*read_res, "plan step 1");
}

TEST(ToolExecutorTest, WriteScratchpadRequiresContent) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;
  executor.SetSessionId("s1");

  auto write_res = executor.Execute("write_scratchpad", nlohmann::json::object());
  EXPECT_FALSE(write_res.ok());
  EXPECT_EQ(write_res.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(ToolExecutorTest, SubqueryScopeRejectsLlmQueryCalls) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  executor.SetExecutionContext(ToolExecutor::ExecutionScope::kSubquery, 1);
  auto res = executor.Execute("llm_query", {{"query", "hi"}});

  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(res.status().message(), "not allowed in subquery scope"));
}

TEST(ToolExecutorTest, SubqueryScopeRejectsSpecializationCalls) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  executor.RegisterTool("llm_tool_reviewer", [](const nlohmann::json&, std::shared_ptr<CancellationRequest>) {
    return absl::StatusOr<std::string>("ok");
  });

  executor.SetExecutionContext(ToolExecutor::ExecutionScope::kSubquery, 1);
  auto res = executor.Execute("llm_tool_reviewer", nlohmann::json::object());

  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(res.status().message(), "not allowed in subquery scope"));
}

TEST(ToolExecutorTest, RootScopeAllowsSpecializationCalls) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  executor.RegisterTool("llm_tool_researcher", [](const nlohmann::json&, std::shared_ptr<CancellationRequest>) {
    return absl::StatusOr<std::string>("ok");
  });

  executor.SetExecutionContext(ToolExecutor::ExecutionScope::kRoot, 0);
  auto res = executor.Execute("llm_tool_researcher", nlohmann::json::object());

  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_EQ(EnvelopeResultText(*res), "ok");
}

TEST(ToolExecutorTest, DepthGreaterThanOneRejected) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  executor.SetExecutionContext(ToolExecutor::ExecutionScope::kSubquery, 2);
  auto res = executor.Execute("list_directory", {{"path", "."}});

  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(res.status().message(), "execution_depth must be <= 1"));
}

TEST(ToolExecutorTest, RunJsRegisteredAndReturnsJsonText) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto result = executor.Execute("run_js", {{"code", "return { ok: true, value: 21 * 2 };"}});

  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_TRUE(absl::StrContains(*result, "\"ok\":true"));
  EXPECT_TRUE(absl::StrContains(*result, "\"value\":42"));
}

TEST(ToolExecutorTest, RunJsRejectsInvalidArgs) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto result = executor.Execute("run_js", {{"code", 42}});

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(ToolExecutorTest, RunJsRejectsUndefinedResult) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto result = executor.Execute("run_js", {{"code", "return undefined;"}});

  ASSERT_FALSE(result.ok());
  EXPECT_TRUE(absl::StrContains(result.status().message(), "not JSON-serializable"));
}

TEST(ToolExecutorTest, RunJsCanCallRegisteredToolThroughBridge) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  executor.RegisterTool("echo_for_js", [](const nlohmann::json& args, std::shared_ptr<CancellationRequest>) {
    EXPECT_EQ(args, nlohmann::json({{"value", 42}}));
    return absl::StatusOr<std::string>(R"({"ok":true,"value":42})");
  });

  ASSERT_TRUE(db.RegisterTool({"echo_for_js", "echo", "{}", true}).ok());
  auto result = executor.Execute("run_js", {{"code", "return tools.echo_for_js({ value: 42 });"}});

  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_TRUE(absl::StrContains(*result, "\"ok\":true"));
  EXPECT_TRUE(absl::StrContains(*result, "\"value\":42"));
}

TEST(ToolExecutorTest, RunJsBridgeRejectsUnknownTool) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto result = executor.Execute("run_js", {{"code", "return call_tool('definitely_missing', {});"}});

  ASSERT_FALSE(result.ok());
  EXPECT_TRUE(absl::StrContains(result.status().message(), "not callable from run_js"));
}

TEST(ToolExecutorTest, RunJsBridgeRejectsNonCallableToolBeforeDispatch) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  ASSERT_TRUE(db.RegisterTool({"blocked_for_js", "blocked", "{}", true, 0, true, false}).ok());
  auto result = executor.Execute("run_js", {{"code", "return tools.blocked_for_js({});"}});

  ASSERT_FALSE(result.ok());
  EXPECT_TRUE(absl::StrContains(result.status().message(), "not callable from run_js"));
}

TEST(ToolExecutorTest, RunJsBridgeRejectsDefaultWorkflowTools) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto ask_result = executor.Execute("run_js", {{"code", "return tools.ask_user({ prompt: 'hidden?' });"}});
  ASSERT_FALSE(ask_result.ok());
  EXPECT_TRUE(absl::StrContains(ask_result.status().message(), "not callable from run_js"));

  auto git_result = executor.Execute("run_js", {{"code", "return tools.git_finalize_series({});"}});
  ASSERT_FALSE(git_result.ok());
  EXPECT_TRUE(absl::StrContains(git_result.status().message(), "not callable from run_js"));
}

TEST(ToolExecutorTest, RunJsBridgePreservesToolValidation) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto result = executor.Execute("run_js", {{"code", "return tools.read_file({});"}});

  ASSERT_FALSE(result.ok());
  EXPECT_TRUE(absl::StrContains(result.status().message(), "path"));
}

TEST(ToolExecutorTest, RunJsBridgeRejectsRecursiveRunJs) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto result = executor.Execute("run_js", {{"code", "return call_tool('run_js', { code: 'return 1;' });"}});

  ASSERT_FALSE(result.ok());
  EXPECT_TRUE(absl::StrContains(result.status().message(), "recursively invoke run_js"));
}

TEST(ToolExecutorTest, RootRunJsBridgeAllowsLlmQueryWhenRegistered) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  executor.RegisterTool("llm_query", [](const nlohmann::json& args, std::shared_ptr<CancellationRequest>) {
    EXPECT_EQ(args, nlohmann::json({{"query", "summarize"}}));
    return absl::StatusOr<std::string>(R"({"summary":"ok"})");
  });

  ASSERT_TRUE(db.RegisterTool({"llm_query", "llm", "{}", true}).ok());
  executor.SetExecutionContext(ToolExecutor::ExecutionScope::kRoot, 0);
  auto result = executor.Execute("run_js", {{"code", "return tools.llm_query({ query: 'summarize' });"}});

  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_TRUE(absl::StrContains(*result, "\"summary\":\"ok\""));
}

TEST(ToolExecutorTest, SubqueryRunJsBridgeRejectsLlmQuery) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  executor.RegisterTool("llm_query", [](const nlohmann::json&, std::shared_ptr<CancellationRequest>) {
    return absl::StatusOr<std::string>("should not run");
  });

  ASSERT_TRUE(db.RegisterTool({"llm_query", "llm", "{}", true}).ok());
  executor.SetExecutionContext(ToolExecutor::ExecutionScope::kSubquery, 1);
  auto result = executor.Execute("run_js", {{"code", "return tools.llm_query({ query: 'blocked' });"}});

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(result.status().message(), "not allowed in subquery scope"));
}

TEST(ToolExecutorTest, SubqueryRunJsBridgeRejectsLlmToolSpecialization) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  executor.RegisterTool("llm_tool_researcher", [](const nlohmann::json&, std::shared_ptr<CancellationRequest>) {
    return absl::StatusOr<std::string>("should not run");
  });

  ASSERT_TRUE(db.RegisterTool({"llm_tool_researcher", "llm", "{}", true}).ok());
  executor.SetExecutionContext(ToolExecutor::ExecutionScope::kSubquery, 1);
  auto result = executor.Execute("run_js", {{"code", "return tools.llm_tool_researcher({ topic: 'blocked' });"}});

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(result.status().message(), "not allowed in subquery scope"));
}

TEST(ToolExecutorTest, SubqueryRunJsBridgeAllowsOrdinaryTool) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  executor.RegisterTool("ordinary_tool", [](const nlohmann::json&, std::shared_ptr<CancellationRequest>) {
    return absl::StatusOr<std::string>(R"({"ordinary":true})");
  });

  ASSERT_TRUE(db.RegisterTool({"ordinary_tool", "ordinary", "{}", true}).ok());
  executor.SetExecutionContext(ToolExecutor::ExecutionScope::kSubquery, 1);
  auto result = executor.Execute("run_js", {{"code", "return tools.ordinary_tool({});"}});

  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_TRUE(absl::StrContains(*result, "\"ordinary\":true"));
}

}  // namespace slop
