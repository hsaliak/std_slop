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
    EXPECT_EQ(*read_res, content);
    
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
    ASSERT_TRUE(executor.Execute("index_directory", {{"path", "."}}).ok());

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

}  // namespace slop
