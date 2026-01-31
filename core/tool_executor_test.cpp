#include "core/tool_executor.h"

#include <filesystem>
#include <fstream>
#include <iostream>

#include <gtest/gtest.h>
#include "absl/strings/match.h"

namespace slop {

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
  EXPECT_TRUE(write_res->find("### TOOL_RESULT: write_file") != std::string::npos);

  auto read_res = executor.Execute("read_file", {{"path", test_file}});
  ASSERT_TRUE(read_res.ok());
  EXPECT_TRUE(read_res->find("### TOOL_RESULT: read_file") != std::string::npos);
  EXPECT_TRUE(read_res->find("1: " + content) != std::string::npos);

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
  EXPECT_TRUE(res1->find("2: Line 2") != std::string::npos);
  EXPECT_TRUE(res1->find("3: Line 3") != std::string::npos);
  EXPECT_TRUE(res1->find("4: Line 4") != std::string::npos);
  EXPECT_TRUE(res1->find("1: Line 1") == std::string::npos);
  EXPECT_TRUE(res1->find("5: Line 5") == std::string::npos);

  // Test: Start only
  auto res2 = executor.Execute("read_file", {{"path", test_file}, {"start_line", 4}});
  ASSERT_TRUE(res2.ok());
  EXPECT_TRUE(res2->find("4: Line 4") != std::string::npos);
  EXPECT_TRUE(res2->find("5: Line 5") != std::string::npos);
  EXPECT_TRUE(res2->find("3: Line 3") == std::string::npos);

  // Test: End only
  auto res3 = executor.Execute("read_file", {{"path", test_file}, {"end_line", 2}});
  ASSERT_TRUE(res3.ok());
  EXPECT_TRUE(res3->find("1: Line 1") != std::string::npos);
  EXPECT_TRUE(res3->find("2: Line 2") != std::string::npos);
  EXPECT_TRUE(res3->find("3: Line 3") == std::string::npos);

  // Test: Out of bounds
  auto res4 = executor.Execute("read_file", {{"path", test_file}, {"start_line", 10}});
  ASSERT_TRUE(res4.ok());
  EXPECT_TRUE(res4->find("10: ") == std::string::npos);

  // Test: Invalid range
  auto res5 = executor.Execute("read_file", {{"path", test_file}, {"start_line", 5}, {"end_line", 2}});
  ASSERT_TRUE(res5.ok());
  EXPECT_TRUE(res5->find("Error: INVALID_ARGUMENT") != std::string::npos);

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
  EXPECT_TRUE(absl::StrContains(*res, "### FILE: test_metadata.txt | TOTAL_LINES: 3 | RANGE: 1-2"));
  EXPECT_TRUE(absl::StrContains(*res, "Use 'read_file' with start_line=3"));

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, GitGrepSummary) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  // Check if we are in a git repo
  auto git_res = executor.Execute("execute_bash", {{"command", "git rev-parse --is-inside-work-tree"}});
  if (!git_res.ok() || git_res->find("true") == std::string::npos) {
    GTEST_SKIP() << "Not in a git repository, skipping GitGrepSummary test";
  }

  std::string test_file = "many_matches.txt";
  std::string content;
  for (int i = 0; i < 30; ++i) content += "match_this_string\n";
  ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", content}}).ok());

  // We need to add the file to git to grep it if it's a new file, or use --no-index
  // Actually git_grep_tool uses git grep. If we want it to work on untracked files, we'd need --no-index.
  // Our git_grep_tool doesn't seem to support --no-index in its args yet.
  
  // Let's just use an existing file that we know has many matches, or git add it.
  (void)executor.Execute("execute_bash", {{"command", "git add " + test_file}});

  auto res = executor.Execute("git_grep_tool", {{"pattern", "match_this_string"}});
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(absl::StrContains(*res, "### SEARCH_SUMMARY:"));
  EXPECT_TRUE(absl::StrContains(*res, "many_matches.txt: 30"));

  (void)executor.Execute("execute_bash", {{"command", "git rm -f " + test_file}});
}

TEST(ToolExecutorTest, ExecuteBash) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("execute_bash", {{"command", "echo 'slop'"}});
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(res->find("### TOOL_RESULT: execute_bash") != std::string::npos);
  EXPECT_TRUE(res->find("slop") != std::string::npos);
}

TEST(ToolExecutorTest, ToolNotFound) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("non_existent", {});
  EXPECT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kNotFound);
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

TEST(ToolExecutorTest, GrepToolWorks) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto write_res =
      executor.Execute("write_file", {{"path", "grep_test.txt"}, {"content", "line 1\npattern here\nline 3"}});
  ASSERT_TRUE(write_res.ok());

  auto grep_res = executor.Execute("grep_tool", {{"pattern", "pattern"}, {"path", "grep_test.txt"}, {"context", 1}});
  ASSERT_TRUE(grep_res.ok());
  EXPECT_TRUE(grep_res->find("pattern here") != std::string::npos);

  std::filesystem::remove("grep_test.txt");
}

TEST(ToolExecutorTest, GrepToolNoMatches) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto write_res = executor.Execute("write_file", {{"path", "grep_empty.txt"}, {"content", "nothing here"}});
  ASSERT_TRUE(write_res.ok());

  auto grep_res = executor.Execute("grep_tool", {{"pattern", "NON_EXISTENT_PATTERN"}, {"path", "grep_empty.txt"}});
  ASSERT_TRUE(grep_res.ok());
  // Should be ok (exit code 1), and NOT contain "Error:"
  EXPECT_TRUE(grep_res->find("### TOOL_RESULT: grep_tool") != std::string::npos);
  EXPECT_TRUE(grep_res->find("Error:") == std::string::npos);

  std::filesystem::remove("grep_empty.txt");
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

TEST(ToolExecutorTest, GitGrepToolWorks) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto git_repo_check = executor.Execute("execute_bash", {{"command", "git rev-parse --is-inside-work-tree"}});
  if (!git_repo_check.ok() || git_repo_check->find("true") == std::string::npos) {
    GTEST_SKIP() << "Not in a git repository, skipping GitGrepToolWorks test";
  }

  // git_grep_tool should work for tracked files in this repo.
  // We search for "GitGrep" which we know is in tool_executor.cpp
  auto grep_res = executor.Execute("git_grep_tool", {{"pattern", "GitGrep"}, {"path", "."}});
  ASSERT_TRUE(grep_res.ok());
  EXPECT_TRUE(grep_res->find("GitGrep") != std::string::npos);
  EXPECT_TRUE(grep_res->find("tool_executor.cpp") != std::string::npos);
}

TEST(ToolExecutorTest, GitGrepToolNoMatches) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto git_repo_check = executor.Execute("execute_bash", {{"command", "git rev-parse --is-inside-work-tree"}});
  if (!git_repo_check.ok() || git_repo_check->find("true") == std::string::npos) {
    GTEST_SKIP() << "Not in a git repository, skipping GitGrepToolNoMatches test";
  }

  auto grep_res = executor.Execute("git_grep_tool", {{"pattern", "NON_EXISTENT_PATTERN_XYZ_123"}, {"path", "."}});
  ASSERT_TRUE(grep_res.ok());
  // Should be ok (exit code 1), and NOT contain "Error:"
  EXPECT_TRUE(grep_res->find("### TOOL_RESULT: git_grep_tool") != std::string::npos);
  EXPECT_TRUE(grep_res->find("Error:") == std::string::npos);
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

TEST(ToolExecutorTest, ManageScratchpadSessionHandling) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  // Without SetSessionId, manage_scratchpad should return an error message in the result string
  auto res = executor.Execute("manage_scratchpad", {{"action", "read"}});
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(res->find("Error: FAILED_PRECONDITION: No active session") != std::string::npos);

  // With SetSessionId, it should work
  executor.SetSessionId("default_session");
  auto res2 = executor.Execute("manage_scratchpad", {{"action", "read"}});
  ASSERT_TRUE(res2.ok());
  EXPECT_TRUE(res2->find("Scratchpad is empty") != std::string::npos);

  // Switch session
  executor.SetSessionId("other_session");
  ASSERT_TRUE(executor.Execute("manage_scratchpad", {{"action", "update"}, {"content", "other content"}}).ok());

  auto res3 = executor.Execute("manage_scratchpad", {{"action", "read"}});
  ASSERT_TRUE(res3.ok());
  EXPECT_TRUE(res3->find("other content") != std::string::npos);
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
  EXPECT_TRUE(res4->find("Double-quote: \"baz\"") != std::string::npos);

  std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, GitGrepAdvancedFeatures) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  // This test assumes it's running in a git repo
  auto git_repo_check = executor.Execute("execute_bash", {{"command", "git rev-parse --is-inside-work-tree"}});
  if (!git_repo_check.ok() || git_repo_check->find("true") == std::string::npos) {
    GTEST_SKIP() << "Not in a git repository, skipping GitGrepAdvancedFeatures test";
  }

  // Test: Multiple patterns
  auto res1 = executor.Execute("git_grep_tool", {{"patterns", {"ToolExecutor", "GitGrep"}}, {"all_match", true}});
  ASSERT_TRUE(res1.ok());
  EXPECT_TRUE(res1->find("core/tool_executor.cpp") != std::string::npos);

  // Test: Multiple pathspecs
  auto res2 = executor.Execute("git_grep_tool", {{"pattern", "TEST"}, {"path", {"core/*.cpp", "interface/*.cpp"}}});
  ASSERT_TRUE(res2.ok());
  EXPECT_TRUE(res2->find("core/tool_executor_test.cpp") != std::string::npos);
  EXPECT_TRUE(res2->find("interface/ui_test.cpp") != std::string::npos);
}

TEST(ToolExecutorTest, GitGrepBooleanExpressions) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto git_repo_check = executor.Execute("execute_bash", {{"command", "git rev-parse --is-inside-work-tree"}});
  if (!git_repo_check.ok() || git_repo_check->find("true") == std::string::npos) {
    GTEST_SKIP() << "Not in a git repository, skipping GitGrepBooleanExpressions test";
  }

  // Test: AND (on the same line)
  // Find lines in core/tool_executor.cpp that contain both "absl" and "StatusOr"
  auto res1 = executor.Execute("git_grep_tool",
                               {{"patterns", {"absl", "--and", "StatusOr"}}, {"path", "core/tool_executor.cpp"}});
  ASSERT_TRUE(res1.ok());
  EXPECT_TRUE(res1->find("absl::StatusOr") != std::string::npos);

  // Test: OR
  // Find lines that contain "ToolExecutor" OR "RetrieveMemos"
  auto res2 = executor.Execute(
      "git_grep_tool", {{"patterns", {"ToolExecutor", "--or", "RetrieveMemos"}}, {"path", "core/tool_executor.cpp"}});
  ASSERT_TRUE(res2.ok());
  // "class ToolExecutor" is in tool_executor.h, not .cpp.
  // In .cpp we have "ToolExecutor::Execute" etc.
  EXPECT_TRUE(res2->find("ToolExecutor::") != std::string::npos);
  EXPECT_TRUE(res2->find("RetrieveMemos") != std::string::npos);

  // Test: Grouping with ( )
  // ( "absl" AND "StatusOr" ) OR "RetrieveMemos"
  auto res3 =
      executor.Execute("git_grep_tool", {{"patterns", {"(", "absl", "--and", "StatusOr", ")", "--or", "RetrieveMemos"}},
                                         {"path", "core/tool_executor.cpp"}});
  ASSERT_TRUE(res3.ok());
  EXPECT_TRUE(res3->find("absl::StatusOr") != std::string::npos);
  EXPECT_TRUE(res3->find("RetrieveMemos") != std::string::npos);
}

}  // namespace slop
