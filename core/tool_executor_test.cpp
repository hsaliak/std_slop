#include "absl/strings/match.h"
#include "core/tool_executor.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "absl/strings/match.h"

#include "core/tool_dispatcher.h"

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
  auto write_env = ParseEnvelope(*write_res);
  ASSERT_TRUE(write_env.is_object());
  EXPECT_EQ(write_env.value("ok", false), true);
  EXPECT_EQ(write_env.value("tool", std::string{}), "write_file");

  auto read_res = executor.Execute("read_file", {{"path", test_file}});
  ASSERT_TRUE(read_res.ok());
  auto read_env = ParseEnvelope(*read_res);
  ASSERT_TRUE(read_env.is_object());
  EXPECT_EQ(read_env.value("ok", false), true);
  EXPECT_EQ(read_env.value("tool", std::string{}), "read_file");
  const std::string read_text = EnvelopeResultText(*read_res);
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
  ASSERT_TRUE(res3.ok());
  const std::string text3 = EnvelopeResultText(*res3);
  EXPECT_TRUE(absl::StrContains(text3, "Line 1\nLine 2\n"));
  EXPECT_TRUE(!absl::StrContains(text3, "3: Line 3"));

  // Test: Out of bounds
  auto res4 = executor.Execute("read_file", {{"path", test_file}, {"start_line", 10}});
  ASSERT_TRUE(res4.ok());
  EXPECT_TRUE(!absl::StrContains(EnvelopeResultText(*res4), "10: "));

  // Test: Invalid range
  auto res5 = executor.Execute("read_file", {{"path", test_file}, {"start_line", 5}, {"end_line", 2}});
  ASSERT_TRUE(res5.ok());
  EXPECT_TRUE(absl::StrContains(EnvelopeResultText(*res5), "Error: INVALID_ARGUMENT"));

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

TEST(ToolExecutorTest, GrepSummary) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string test_file = "many_matches.txt";
  std::string content;
  for (int i = 0; i < 30; ++i) content += "match_this_string\n";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", content}}).ok());

  auto res = executor.Execute("grep_tool", {{"pattern", "match_this_string"}, {"path", test_file}});
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(res->find("match_this_string") != std::string::npos);

  std::filesystem::remove(test_file);
}
TEST(ToolExecutorTest, ExecuteBash) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("execute_bash", {{"command", "echo 'slop'"}});
  ASSERT_TRUE(res.ok());
  auto env = ParseEnvelope(*res);
  ASSERT_TRUE(env.is_object());
  std::cout << "RES: " << *res << std::endl;
  EXPECT_EQ(env.value("ok", false), true);
  EXPECT_EQ(env.value("tool", std::string{}), "execute_bash");
  EXPECT_TRUE(res->find("slop") != std::string::npos);
}

TEST(ToolExecutorTest, ToolNotFound) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("non_existent", {});
  ASSERT_TRUE(res.ok());
  auto env = ParseEnvelope(*res);
  ASSERT_TRUE(env.is_object());
  EXPECT_EQ(env.value("ok", true), false);
  EXPECT_EQ(env.value("tool", std::string{}), "non_existent");
  ASSERT_TRUE(env.contains("error"));
  EXPECT_TRUE(absl::StrContains(EnvelopeResultText(*res), "NOT_FOUND"));
}

TEST(ToolExecutorTest, AliasToolNameResolvesToCanonical) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("list_dir", {{"path", "."}, {"depth", 1}});
  ASSERT_TRUE(res.ok());

  auto env = ParseEnvelope(*res);
  ASSERT_TRUE(env.is_object());
  EXPECT_EQ(env.value("ok", false), true);
  EXPECT_EQ(env.value("tool", std::string{}), "list_directory");
  EXPECT_EQ(env.value("requested_tool", std::string{}), "list_dir");
  EXPECT_EQ(env.value("alias_used", false), true);
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

TEST(ToolExecutorTest, RunJsAcceptsNestedScriptField) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;
  executor.SetSessionId("nested_run_js_test");

  nlohmann::json args = {
      {"args", {{"script", "return args.value;"}, {"value", "ok"}}},
  };
  auto res = executor.Execute("run_js", args);
  ASSERT_TRUE(res.ok()) << res.status().ToString();
  EXPECT_TRUE(absl::StrContains(*res, "ok"));
}

TEST(ToolExecutorTest, GrepToolWorks) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string test_file = "grep_repo.txt";
  std::string content = "needle\n";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", content}}).ok());

  auto grep_res = executor.Execute("grep_tool", {{"pattern", "needle"}, {"path", test_file}});
  ASSERT_TRUE(grep_res.ok());
  EXPECT_TRUE(grep_res->find("needle") != std::string::npos);

  std::filesystem::remove(test_file);
}
TEST(ToolExecutorTest, GrepToolNoMatches) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string test_file = "grep_repo_empty.txt";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", "haystack"}}).ok());

  auto grep_res = executor.Execute("grep_tool", {{"pattern", "NON_EXISTENT_PATTERN_XYZ_123"}, {"path", test_file}});
  ASSERT_TRUE(grep_res.ok());
  auto grep_env = ParseEnvelope(*grep_res);
  ASSERT_TRUE(grep_env.is_object());
  EXPECT_EQ(grep_env.value("ok", false), true);
  EXPECT_EQ(grep_env.value("tool", std::string{}), "grep_tool");
  EXPECT_TRUE(!absl::StrContains(EnvelopeResultText(*grep_res), "Error:"));

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, GrepToolSimplifiedArguments) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string test_file = "grep_multi.txt";
  std::string content = "alpha\nbeta\nalpha beta\n";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", content}}).ok());

  auto res1 = executor.Execute("grep_tool", {{"pattern", "alpha"}, {"path", test_file}});
  ASSERT_TRUE(res1.ok());
  EXPECT_TRUE(res1->find("alpha") != std::string::npos);

  auto res2 = executor.Execute("grep_tool", {{"pattern", "beta"}, {"path", test_file}});
  ASSERT_TRUE(res2.ok());

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, GrepToolContextWorks) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string test_file = "grep_context.txt";
  std::string content = "line1\nmatch\nline3\n";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", content}}).ok());

  auto res = executor.Execute("grep_tool", {{"pattern", "match"}, {"path", test_file}, {"context", 1}});
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(res->find("line1") != std::string::npos);
  EXPECT_TRUE(res->find("match") != std::string::npos);
  EXPECT_TRUE(res->find("line3") != std::string::npos);

  std::filesystem::remove(test_file);
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
  const std::string patch_text = EnvelopeResultText(*patch_res);
  EXPECT_TRUE(absl::StrContains(patch_text, "\"ok\":false"));
  EXPECT_TRUE(absl::StrContains(patch_text, "PATCH_DRY_RUN_FAILED"));

  auto read_res = executor.Execute("read_file", {{"path", test_file}});
  ASSERT_TRUE(read_res.ok());
  EXPECT_TRUE(!absl::StrContains(EnvelopeResultText(*read_res), "replacement"));

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, GrepToolEscaping) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string test_file = "grep_escape_test.txt";
  std::string content =
      "Normal line\nDash-line: ---\nQuote-line: 'foo bar'\nDouble-quote: \"baz\"\n-starting-with-dash";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", content}}).ok());

  // Test: Triple dash
  auto res1 = executor.Execute("grep_tool", {{"pattern", "---"}, {"path", test_file}});
  ASSERT_TRUE(res1.ok());
  EXPECT_TRUE(res1->find("Dash-line: ---") != std::string::npos);

  // Test: Pattern starting with dash
  auto res2 = executor.Execute("grep_tool", {{"pattern", "-starting"}, {"path", test_file}});
  ASSERT_TRUE(res2.ok());
  EXPECT_TRUE(res2->find("-starting-with-dash") != std::string::npos);

  // Test: Single quote
  auto res3 = executor.Execute("grep_tool", {{"pattern", "'foo bar'"}, {"path", test_file}});
  ASSERT_TRUE(res3.ok());
  EXPECT_TRUE(res3->find("Quote-line: 'foo bar'") != std::string::npos);

  // Test: Double quote
  auto res4 = executor.Execute("grep_tool", {{"pattern", "\"baz\""}, {"path", test_file}});
  ASSERT_TRUE(res4.ok());
  EXPECT_TRUE(absl::StrContains(EnvelopeResultText(*res4), "Double-quote: \"baz\""));

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, RunJsBasic) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  // Simple script that calls another tool
  std::string script = R"(
    const res = tools.execute_bash({command: "echo 'hello from js'"});
    print("Bash said: " + res);
    return "js_done";
  )";

  auto res = executor.Execute("run_js", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(res->find("hello from js") != std::string::npos);
  EXPECT_TRUE(res->find("js_done") != std::string::npos);
}

TEST(ToolExecutorTest, RunJsExecuteBashProvidesStructuredFields) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string script = R"(
    const res = tools.execute_bash({command: "echo 'structured shell output'"});
    return res;
  )";

  auto res = executor.Execute("run_js", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "\"stdout\":\"structured shell output\\n\""));
  EXPECT_TRUE(absl::StrContains(*res, "\"stderr\":\"\""));
  EXPECT_TRUE(absl::StrContains(*res, "\"exit_code\":0"));
}

TEST(ToolExecutorTest, RunJsExecuteBashExposesMetadataAndStringBehavior) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string script = R"(
    const res = tools.execute_bash({command: "echo 'hybrid shell output'"});
    return {
      stdout: res.stdout,
      stderr: res.stderr,
      exit_code: res.exit_code,
      exitCode: res.exitCode,
      split_head: res.output.split("\n")[0],
      has_output: res.output.includes("hybrid shell output")
    };
  )";

  auto res = executor.Execute("run_js", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "\"stdout\":\"hybrid shell output\\n\""));
  EXPECT_TRUE(absl::StrContains(*res, "\"exit_code\":0"));
  EXPECT_TRUE(absl::StrContains(*res, "\"exitCode\":0"));
  EXPECT_TRUE(absl::StrContains(*res, "\"split_head\":\"hybrid shell output\""));
  EXPECT_TRUE(absl::StrContains(*res, "\"has_output\":true"));
}

TEST(ToolExecutorTest, RunJsReturnsJsonForObjects) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string script = R"(
    return {
      ok: true,
      count: 2,
      nested: { value: "x" },
      list: [1, 2]
    };
  )";

  auto res = executor.Execute("run_js", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "\"ok\":true"));
  EXPECT_TRUE(absl::StrContains(*res, "\"nested\":{\"value\":\"x\"}"));
  EXPECT_TRUE(absl::StrContains(*res, "\"list\":[1,2]"));
}

TEST(ToolExecutorTest, RunJsFullSpectrum) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;
  // Script that iterates over the tools table
  std::string script = R"(
    let count = 0;
    for (const name in tools) {
      count++;
      print("Found tool: " + name);
      if (typeof tools[name] !== "function") throw new Error("Not a function");
    }
    print("Total tools: " + count);
    return count.toString();
  )";
  auto res = executor.Execute("run_js", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(res->find("Found tool: execute_bash") != std::string::npos);
  EXPECT_TRUE(res->find("Found tool: read_file") != std::string::npos);
  EXPECT_TRUE(res->find("Total tools: ") != std::string::npos);
}

TEST(ToolExecutorTest, RunJsPreamble) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string script = R"(
    if (typeof tools.execute_bash !== "function") throw new Error("execute_bash not found");
    if (typeof core.resolve_tool_name !== "function") throw new Error("core.resolve_tool_name not found");
    if (core.resolve_tool_name("shell") !== "execute_bash") throw new Error("alias resolution failed");
    return "preamble_ok";
  )";

  auto res = executor.Execute("run_js", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(res->find("preamble_ok") != std::string::npos);
}

TEST(ToolExecutorTest, RunJsFailsWhenNoOutputProduced) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("run_js", {{"script", "const value = 1 + 1;"}});
  ASSERT_FALSE(res.ok());
  EXPECT_TRUE(absl::StrContains(res.status().message(),
                                "run_js produced no output: script must return a value or print output"));
}

TEST(ToolExecutorTest, ConsoleAndStdOsAreAvailable) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto dispatcher = std::make_unique<ToolDispatcher>([&executor](const std::string& name, const nlohmann::json& args,
                                                                 std::shared_ptr<CancellationRequest> cancellation) {
    return executor.Execute(name, args, cancellation);
  });
  executor.SetDispatcher(std::move(dispatcher));

  std::string script = R"(
    if (typeof console !== "object") throw new Error("console missing");
    console.log("console-ok");
    if (typeof std !== "object") throw new Error("std missing");
    if (typeof os !== "object") throw new Error("os missing");
    return std !== undefined && os !== undefined;
  )";

  auto res = executor.Execute("run_js", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "console-ok"));
  EXPECT_TRUE(absl::StrContains(*res, "true"));
}

TEST(ToolExecutorTest, AsyncJobExecution) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto dispatcher = std::make_unique<ToolDispatcher>([&executor](const std::string& name, const nlohmann::json& args,
                                                                 std::shared_ptr<CancellationRequest> cancellation) {
    return executor.Execute(name, args, cancellation);
  });
  executor.SetDispatcher(std::move(dispatcher));

  std::string script = R"(
    const job = tools.dispatch_async("execute_bash", {command: "echo 'hello async'"});
    if (typeof job !== "object") throw new Error("job is not object, got " + typeof job);
    const result = job.wait();
    return result;
  )";

  auto res = executor.Execute("run_js", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "hello async"));
}

TEST(ToolExecutorTest, AsyncJobParallelism) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto dispatcher = std::make_unique<ToolDispatcher>([&executor](const std::string& name, const nlohmann::json& args,
                                                                 std::shared_ptr<CancellationRequest> cancellation) {
    return executor.Execute(name, args, cancellation);
  });
  executor.SetDispatcher(std::move(dispatcher));

  std::string script = R"(
    const job1 = tools.dispatch_async("execute_bash", {command: "sleep 0.1 && echo 'job1'"});
    const job2 = tools.dispatch_async("execute_bash", {command: "sleep 0.1 && echo 'job2'"});
    const res1 = job1.wait();
    const res2 = job2.wait();
    return res1 + " " + res2;
  )";

  auto res = executor.Execute("run_js", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "job1"));
  EXPECT_TRUE(absl::StrContains(*res, "job2"));
}

TEST(ToolExecutorTest, ToolOrchestrationScenario) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto dispatcher = std::make_unique<ToolDispatcher>([&executor](const std::string& name, const nlohmann::json& args,
                                                                 std::shared_ptr<CancellationRequest> cancellation) {
    return executor.Execute(name, args, cancellation);
  });
  executor.SetDispatcher(std::move(dispatcher));

  std::string script = R"(
    tools.write_file({path: 'test_orch.txt', content: 'hello world'});
    const job = tools.dispatch_async("grep", {path: 'test_orch.txt', pattern: 'world'});
    const res2 = job.wait();
    return res2;
  )";

  auto res = executor.Execute("run_js", {{"script", script}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "hello world"));
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

}  // namespace slop
