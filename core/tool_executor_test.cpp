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
      {"patch_tool", {{"path", "mm_guard.txt"}, {"unified_diff", "--- a\n+++ a\n@@ -1 +1 @@\n-x\n+y\n"}}},
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
      "git_reroll_patch", "patch_tool",          "write_file",
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
      "query_db",
      "read_file",
      "read_scratchpad",
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

TEST(ToolExecutorTest, ApplyPatch_Success) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string test_file = "patch_success.txt";
  std::string initial_content = "void function1() {\n  // First\n}\n\nvoid function2() {\n  // Second\n}\n";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", initial_content}}).ok());

  std::string unified_diff =
      "--- patch_success.txt\n"
      "+++ patch_success.txt\n"
      "@@ -4,4 +4,4 @@\n"
      " \n"
      " void function2() {\n"
      "-  // Second\n"
      "+  // Updated Second\n"
      " }\n";

  auto patch_res = executor.Execute("patch_tool", {{"path", test_file}, {"unified_diff", unified_diff}});
  ASSERT_TRUE(patch_res.ok()) << patch_res.status().message();
  const std::string patch_text = EnvelopeResultText(*patch_res);
  EXPECT_TRUE(absl::StrContains(patch_text, R"("ok":true)"));
  EXPECT_TRUE(absl::StrContains(patch_text, R"("applied":1)"));

  auto read_res = executor.Execute("read_file", {{"path", test_file}});
  ASSERT_TRUE(read_res.ok());
  EXPECT_TRUE(absl::StrContains(EnvelopeResultText(*read_res), "// Updated Second"));

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, ApplyPatch_DryRun) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string test_file = "patch_dry_run.txt";
  std::string initial_content = "void function1() {\n  // First\n}\n";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", initial_content}}).ok());

  std::string unified_diff =
      "--- patch_dry_run.txt\n"
      "+++ patch_dry_run.txt\n"
      "@@ -1,3 +1,3 @@\n"
      " void function1() {\n"
      "-  // First\n"
      "+  // Updated First\n"
      " }\n";

  auto patch_res =
      executor.Execute("patch_tool", {{"path", test_file}, {"unified_diff", unified_diff}, {"dry_run", true}});
  ASSERT_TRUE(patch_res.ok()) << patch_res.status().message();
  const std::string patch_text = EnvelopeResultText(*patch_res);
  EXPECT_TRUE(absl::StrContains(patch_text, R"("ok":true)"));
  EXPECT_TRUE(absl::StrContains(patch_text, R"("can_apply":true)"));
  EXPECT_TRUE(absl::StrContains(patch_text, R"("mode":"dry_run")"));

  auto read_res = executor.Execute("read_file", {{"path", test_file}});
  ASSERT_TRUE(read_res.ok());
  EXPECT_TRUE(!absl::StrContains(EnvelopeResultText(*read_res), "// Updated First"));

  std::filesystem::remove(test_file);
}
TEST(ToolExecutorTest, ApplyPatchUnifiedDiff) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string test_file = "patch_unified.txt";
  std::string initial_content = "alpha\nbeta\ngamma\n";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", initial_content}}).ok());

  std::string unified_diff =
      "--- patch_unified.txt\n"
      "+++ patch_unified.txt\n"
      "@@ -1,3 +1,3 @@\n"
      " alpha\n"
      "-beta\n"
      "+beta patched\n"
      " gamma\n";

  auto dry_res = executor.Execute(
      "patch_tool",
      {{"path", test_file}, {"unified_diff", unified_diff}, {"dry_run", true}, {"ignore_whitespace", true}});
  ASSERT_TRUE(dry_res.ok()) << dry_res.status().message();
  const std::string dry_text = EnvelopeResultText(*dry_res);
  EXPECT_TRUE(absl::StrContains(dry_text, R"("ok":true)"));
  EXPECT_TRUE(absl::StrContains(dry_text, "dry_run"));
  EXPECT_TRUE(absl::StrContains(dry_text, R"("can_apply":true)"));

  auto apply_res = executor.Execute("patch_tool",
                                    {{"path", test_file}, {"unified_diff", unified_diff}, {"ignore_whitespace", true}});
  ASSERT_TRUE(apply_res.ok()) << apply_res.status().message();
  const std::string apply_text = EnvelopeResultText(*apply_res);
  EXPECT_TRUE(absl::StrContains(apply_text, R"("ok":true)"));
  EXPECT_TRUE(absl::StrContains(apply_text, R"("mode":"apply")"));

  auto read_res = executor.Execute("read_file", {{"path", test_file}});
  ASSERT_TRUE(read_res.ok());
  EXPECT_TRUE(absl::StrContains(EnvelopeResultText(*read_res), "beta patched"));

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, ApplyPatch_IgnoresLineNumbersAndWhitespace) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string test_file = "patch_relaxed_match.txt";
  std::string initial_content =
      "alpha\n"
      "beta    value\n"
      "gamma\n";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", initial_content}}).ok());

  // Deliberately wrong line numbers and different spacing in the removal line.
  std::string unified_diff =
      "--- patch_relaxed_match.txt\n"
      "+++ patch_relaxed_match.txt\n"
      "@@ -90,5 +90,5 @@\n"
      " alpha\n"
      "-beta value\n"
      "+beta patched value\n"
      " gamma\n";

  auto apply_res = executor.Execute("patch_tool",
                                    {{"path", test_file}, {"unified_diff", unified_diff}, {"ignore_whitespace", true}});
  ASSERT_TRUE(apply_res.ok()) << apply_res.status().message();
  const std::string apply_text = EnvelopeResultText(*apply_res);
  EXPECT_TRUE(absl::StrContains(apply_text, "\"ok\":true"));
  EXPECT_TRUE(absl::StrContains(apply_text, "\"mode\":\"apply\""));

  auto read_res = executor.Execute("read_file", {{"path", test_file}});
  ASSERT_TRUE(read_res.ok());
  EXPECT_TRUE(absl::StrContains(EnvelopeResultText(*read_res), "beta patched value"));

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, ApplyPatch_DryRunFailureForUnmatchedHunk) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string test_file = "patch_unmatched.txt";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", "alpha\nbeta\n"}}).ok());

  std::string unified_diff =
      "--- patch_unmatched.txt\n"
      "+++ patch_unmatched.txt\n"
      "@@ -1,2 +1,2 @@\n"
      "-does-not-exist\n"
      "+replacement\n";

  auto patch_res =
      executor.Execute("patch_tool", {{"path", test_file}, {"unified_diff", unified_diff}, {"dry_run", true}});
  ASSERT_TRUE(patch_res.ok()) << patch_res.status().message();
  const std::string patch_text = *patch_res;
  EXPECT_TRUE(absl::StrContains(patch_text, "\"ok\":false"));
  EXPECT_TRUE(absl::StrContains(patch_text, "PATCH_DRY_RUN_FAILED")) << patch_text;

  auto read_res = executor.Execute("read_file", {{"path", test_file}});
  ASSERT_TRUE(read_res.ok());
  EXPECT_TRUE(!absl::StrContains(EnvelopeResultText(*read_res), "replacement"));

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

}  // namespace slop
