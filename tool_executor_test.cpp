#include "tool_executor.h"
#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>
#include <iostream>

namespace slop {

TEST(ToolExecutorTest, ReadWriteFile) {
    Database db;
    ASSERT_TRUE(db.Init(":memory:").ok());
    ToolExecutor executor(&db);

    std::string test_file = "test_executor.txt";
    std::string content = "Hello from ToolExecutor";

    auto write_res = executor.Execute("write_file", {{"path", test_file}, {"content", content}});
    ASSERT_TRUE(write_res.ok());
    EXPECT_TRUE(write_res->find("---TOOL_RESULT: write_file---") != std::string::npos);

    auto read_res = executor.Execute("read_file", {{"path", test_file}});
    ASSERT_TRUE(read_res.ok());
    EXPECT_TRUE(read_res->find("---TOOL_RESULT: read_file---") != std::string::npos);
    EXPECT_TRUE(read_res->find("1: " + content) != std::string::npos);

    std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, ExecuteBash) {
    Database db;
    ASSERT_TRUE(db.Init(":memory:").ok());
    ToolExecutor executor(&db);

    auto res = executor.Execute("execute_bash", {{"command", "echo 'slop'"}});
    ASSERT_TRUE(res.ok());
    EXPECT_TRUE(res->find("---TOOL_RESULT: execute_bash---") != std::string::npos);
    EXPECT_TRUE(res->find("slop") != std::string::npos);
}

TEST(ToolExecutorTest, ToolNotFound) {
    Database db;
    ASSERT_TRUE(db.Init(":memory:").ok());
    ToolExecutor executor(&db);

    auto res = executor.Execute("non_existent", {});
    EXPECT_FALSE(res.ok());
    EXPECT_EQ(res.status().code(), absl::StatusCode::kNotFound);
}

TEST(ToolExecutorTest, IndexAndSearch) {
    Database db;
    ASSERT_TRUE(db.Init(":memory:").ok());
    ToolExecutor executor(&db);

    ASSERT_TRUE(executor.Execute("write_file", {{"path", "search_test.cpp"}, {"content", "void slop_function() {}"}}).ok());

    auto search_res = executor.Execute("search_code", {{"query", "slop_function"}});
    ASSERT_TRUE(search_res.ok());
    EXPECT_TRUE(search_res->find("search_test.cpp") != std::string::npos);

    std::filesystem::remove("search_test.cpp");
}

TEST(ToolExecutorTest, QueryDb) {
    Database db;
    ASSERT_TRUE(db.Init(":memory:").ok());
    ToolExecutor executor(&db);

    auto res = executor.Execute("query_db", {{"sql", "SELECT 1 as val"}});
    ASSERT_TRUE(res.ok());
    EXPECT_TRUE(res->find("\"val\":1") != std::string::npos);
}


TEST(ToolExecutorTest, GrepToolWorks) {
    Database db;
    ASSERT_TRUE(db.Init(":memory:").ok());
    ToolExecutor executor(&db);

    auto write_res = executor.Execute("write_file", {{"path", "grep_test.txt"}, {"content", "line 1\npattern here\nline 3"}});
    ASSERT_TRUE(write_res.ok());

    auto grep_res = executor.Execute("grep_tool", {{"pattern", "pattern"}, {"path", "grep_test.txt"}, {"context", 1}});
    ASSERT_TRUE(grep_res.ok());
    EXPECT_TRUE(grep_res->find("pattern here") != std::string::npos);

    std::filesystem::remove("grep_test.txt");
}

TEST(ToolExecutorTest, GitGrepToolWorks) {
    Database db;
    ASSERT_TRUE(db.Init(":memory:").ok());
    ToolExecutor executor(&db);

    // git_grep_tool should work for tracked files in this repo.
    // We search for "GitGrep" which we know is in tool_executor.cpp
    // Since we are in build/, we specify ".." as path.
    auto grep_res = executor.Execute("git_grep_tool", {{"pattern", "GitGrep"}, {"path", ".."}});
    if (grep_res.ok() && grep_res->find("Error:") == std::string::npos) {
        EXPECT_TRUE(grep_res->find("GitGrep") != std::string::npos);
        EXPECT_TRUE(grep_res->find("tool_executor.cpp") != std::string::npos);
    } else {
        // If git is not available, it should at least return a message (handled gracefully)
        EXPECT_FALSE(grep_res->empty());
    }
}

TEST(ToolExecutorTest, ApplyPatch_Success) {
    Database db;
    ASSERT_TRUE(db.Init(":memory:").ok());
    ToolExecutor executor(&db);

    std::string test_file = "patch_success.txt";
    std::string initial_content = "void function1() {\n  // First\n}\n\nvoid function2() {\n  // Second\n}\n";
    ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", initial_content}}).ok());

    nlohmann::json patches = nlohmann::json::array();
    patches.push_back({
        {"find", "void function2() {\n  // Second\n}"},
        {"replace", "void function2() {\n  // Updated Second\n}"}
    });

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
    ToolExecutor executor(&db);

    std::string test_file = "patch_not_found.txt";
    std::string initial_content = "some content\n";
    ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", initial_content}}).ok());

    nlohmann::json patches = nlohmann::json::array();
    patches.push_back({
        {"find", "missing string"},
        {"replace", "replacement"}
    });

    auto patch_res = executor.Execute("apply_patch", {{"path", test_file}, {"patches", patches}});
    ASSERT_TRUE(patch_res.ok());
    EXPECT_TRUE(patch_res->find("Error: NOT_FOUND") != std::string::npos);

    std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, ApplyPatch_AmbiguousMatch) {
    Database db;
    ASSERT_TRUE(db.Init(":memory:").ok());
    ToolExecutor executor(&db);

    std::string test_file = "patch_ambiguous.txt";
    std::string initial_content = "duplicate\nduplicate\n";
    ASSERT_TRUE(executor.Execute("write_file", {{"path", test_file}, {"content", initial_content}}).ok());

    nlohmann::json patches = nlohmann::json::array();
    patches.push_back({
        {"find", "duplicate"},
        {"replace", "unique"}
    });

    auto patch_res = executor.Execute("apply_patch", {{"path", test_file}, {"patches", patches}});
    ASSERT_TRUE(patch_res.ok());
    EXPECT_TRUE(patch_res->find("Error: FAILED_PRECONDITION") != std::string::npos);

    std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, ApplyPatch_MultiplePatches) {
    Database db;
    ASSERT_TRUE(db.Init(":memory:").ok());
    ToolExecutor executor(&db);

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
    ToolExecutor executor(&db);

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

}  // namespace slop
