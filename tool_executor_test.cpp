#include "tool_executor.h"
#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>

namespace slop {

TEST(ToolExecutorTest, ReadWriteFile) {
    Database db;
    ASSERT_TRUE(db.Init(":memory:").ok());
    ToolExecutor executor(&db);
    
    std::string test_file = "test_executor.txt";
    std::string content = "Hello from ToolExecutor";
    
    auto write_res = executor.Execute("write_file", {{"path", test_file}, {"content", content}});
    ASSERT_TRUE(write_res.ok());
    
    auto read_res = executor.Execute("read_file", {{"path", test_file}});
    ASSERT_TRUE(read_res.ok());
    EXPECT_EQ(*read_res, "1: " + content + "\n");
    
    std::filesystem::remove(test_file);
}

TEST(ToolExecutorTest, ExecuteBash) {
    Database db;
    ASSERT_TRUE(db.Init(":memory:").ok());
    ToolExecutor executor(&db);
    
    auto res = executor.Execute("execute_bash", {{"command", "echo 'slop'"}});
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(*res, "slop\n");
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
    EXPECT_TRUE(res->find("\"val\": 1") != std::string::npos);
}


TEST(ToolExecutorTest, GrepToolWorks) {
    Database db;
    ASSERT_TRUE(db.Init(":memory:").ok());
    ToolExecutor executor(&db);
    
    executor.Execute("write_file", {{"path", "grep_test.txt"}, {"content", "line 1\npattern here\nline 3"}});
    
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

}  // namespace slop
