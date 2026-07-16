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



}  // namespace slop
