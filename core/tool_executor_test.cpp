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
    return nlohmann::json();
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
  EXPECT_TRUE(read_text.find("1: ") == std::string::npos);

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
  EXPECT_TRUE(text1.find("Line 2\nLine 3\nLine 4\n") != std::string::npos);
  EXPECT_TRUE(text1.find("1: Line 1") == std::string::npos);
  EXPECT_TRUE(text1.find("5: Line 5") == std::string::npos);

  // Test: Start only
  auto res2 = executor.Execute("read_file", {{"path", test_file}, {"start_line", 4}});
  ASSERT_TRUE(res2.ok());
  const std::string text2 = EnvelopeResultText(*res2);
  EXPECT_TRUE(text2.find("Line 4\nLine 5\n") != std::string::npos);
  EXPECT_TRUE(text2.find("3: Line 3") == std::string::npos);

  // Test: End only
  auto res3 = executor.Execute("read_file", {{"path", test_file}, {"end_line", 2}});
  ASSERT_TRUE(res3.ok());
  const std::string text3 = EnvelopeResultText(*res3);
  EXPECT_TRUE(text3.find("Line 1\nLine 2\n") != std::string::npos);
  EXPECT_TRUE(text3.find("3: Line 3") == std::string::npos);

  // Test: Out of bounds
  auto res4 = executor.Execute("read_file", {{"path", test_file}, {"start_line", 10}});
  ASSERT_TRUE(res4.ok());
  EXPECT_TRUE(EnvelopeResultText(*res4).find("10: ") == std::string::npos);

  // Test: Invalid range
  auto res5 = executor.Execute("read_file", {{"path", test_file}, {"start_line", 5}, {"end_line", 2}});
  ASSERT_TRUE(res5.ok());
  EXPECT_TRUE(EnvelopeResultText(*res5).find("Error: INVALID_ARGUMENT") != std::string::npos);

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
  EXPECT_TRUE(EnvelopeResultText(*res).find("NOT_FOUND") != std::string::npos);
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
  EXPECT_TRUE(EnvelopeResultText(*grep_res).find("Error:") == std::string::npos);

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

  nlohmann::json patches = nlohmann::json::array();
  patches.push_back(
      {{"find", "void function2() {\n  // Second\n}"}, {"replace", "void function2() {\n  // Updated Second\n}"}});

  auto patch_res = executor.Execute("apply_patch", {{"path", test_file}, {"patches", patches}});
  ASSERT_TRUE(patch_res.ok()) << patch_res.status().message();
  EXPECT_TRUE(patch_res->find("Error:") == std::string::npos) << *patch_res;

  auto read_res = executor.Execute("read_file", {{"path", test_file}});
  ASSERT_TRUE(read_res.ok());
  EXPECT_TRUE(read_res->find("Updated Second") != std::string::npos);
  EXPECT_TRUE(read_res->find("function1") != std::string::npos);

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, ApplyPatch_FindNotFound) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string test_file = "patch_not_found.txt";
  std::string initial_content = "some content\n";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", initial_content}}).ok());

  nlohmann::json patches = nlohmann::json::array();
  patches.push_back({{"find", "missing string"}, {"replace", "replacement"}});

  auto patch_res = executor.Execute("apply_patch", {{"path", test_file}, {"patches", patches}});
  ASSERT_TRUE(patch_res.ok());
  EXPECT_TRUE(patch_res->find("Error: NOT_FOUND") != std::string::npos);

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, ApplyPatch_AmbiguousMatch) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string test_file = "patch_ambiguous.txt";
  std::string initial_content = "duplicate\nduplicate\n";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", initial_content}}).ok());

  nlohmann::json patches = nlohmann::json::array();
  patches.push_back({{"find", "duplicate"}, {"replace", "unique"}});

  auto patch_res = executor.Execute("apply_patch", {{"path", test_file}, {"patches", patches}});
  ASSERT_TRUE(patch_res.ok());
  EXPECT_TRUE(patch_res->find("Error: FAILED_PRECONDITION") != std::string::npos);

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, ApplyPatch_MultiplePatches) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string test_file = "patch_multiple.txt";
  std::string initial_content = "line1\nline2\nline3\n";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", initial_content}}).ok());

  nlohmann::json patches = nlohmann::json::array();
  patches.push_back({{"find", "line1"}, {"replace", "part1"}});
  patches.push_back({{"find", "line3"}, {"replace", "part3"}});

  auto patch_res = executor.Execute("apply_patch", {{"path", test_file}, {"patches", patches}});
  ASSERT_TRUE(patch_res.ok());

  auto read_res = executor.Execute("read_file", {{"path", test_file}});
  ASSERT_TRUE(read_res.ok());
  EXPECT_TRUE(read_res->find("part1") != std::string::npos);
  EXPECT_TRUE(read_res->find("line2") != std::string::npos);
  EXPECT_TRUE(read_res->find("part3") != std::string::npos);

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, ApplyPatch_WhitespaceSensitivity) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::string test_file = "patch_whitespace.txt";
  std::string initial_content = "  indented\n";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", initial_content}}).ok());

  // Try to find with wrong indentation
  nlohmann::json patches = nlohmann::json::array();
  patches.push_back({{"find", "indented"}, {"replace", "fixed"}});

  auto patch_res = executor.Execute("apply_patch", {{"path", test_file}, {"patches", patches}});
  ASSERT_TRUE(patch_res.ok());

  auto read_res = executor.Execute("read_file", {{"path", test_file}});
  ASSERT_TRUE(read_res.ok());
  EXPECT_TRUE(read_res->find("  fixed") != std::string::npos);

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, UseSkill) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;
  executor.SetSessionId("s1");
  // Ensure session exists
  ASSERT_TRUE(db.SetContextWindow("s1", 10).ok());

  // Setup a skill
  Database::Skill s;
  s.name = "test_skill";
  s.system_prompt_patch = "TEST PATCH";
  ASSERT_TRUE(db.RegisterSkill(s).ok());

  // Test Activation
  auto res = executor.Execute("use_skill", {{"name", "test_skill"}, {"action", "activate"}});
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(res->find("Skill 'test_skill' activated.") != std::string::npos);
  EXPECT_TRUE(res->find("TEST PATCH") != std::string::npos);

  // Verify DB state
  auto skills = db.GetSkills();
  ASSERT_TRUE(skills.ok());
  EXPECT_EQ((*skills)[skills->size() - 1].activation_count, 1);

  auto active = db.GetActiveSkills("s1");
  ASSERT_TRUE(active.ok());
  ASSERT_EQ(active->size(), 1);
  EXPECT_EQ((*active)[0], "test_skill");

  // Test Deactivation
  auto res2 = executor.Execute("use_skill", {{"name", "test_skill"}, {"action", "deactivate"}});
  ASSERT_TRUE(res2.ok());
  EXPECT_TRUE(res2->find("Skill 'test_skill' deactivated.") != std::string::npos);

  // Verify DB state
  active = db.GetActiveSkills("s1");
  ASSERT_TRUE(active.ok());
  EXPECT_TRUE(active->empty());

  // Activation count should NOT have increased on deactivation
  skills = db.GetSkills();
  ASSERT_TRUE(skills.ok());
  EXPECT_EQ((*skills)[skills->size() - 1].activation_count, 1);
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
  EXPECT_TRUE(EnvelopeResultText(*res4).find("Double-quote: \"baz\"") != std::string::npos);

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
  std::string received_prompt = "";

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


