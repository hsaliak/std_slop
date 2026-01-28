#include "core/tool_executor.h"

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

namespace slop {

class ContextManagementTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(db_.Init(":memory:").ok());
    // Create a dummy session for scratchpad tests
    ASSERT_TRUE(db_.Query("INSERT INTO sessions (id, name) VALUES ('test-session', 'Test Session')").ok());
  }

  Database db_;
};

TEST_F(ContextManagementTest, ListDirectoryBasic) {
  auto executor_or = ToolExecutor::Create(&db_);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  // Create a dummy directory structure
  std::filesystem::create_directory("test_dir");
  std::ofstream("test_dir/file1.txt") << "content";
  std::ofstream("test_dir/file2.txt") << "content";
  std::filesystem::create_directory("test_dir/subdir");
  std::ofstream("test_dir/subdir/subfile.txt") << "content";

  auto res = executor.Execute("list_directory", {{"path", "test_dir"}, {"depth", 1}});
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(res->find("File: file1.txt") != std::string::npos);
  EXPECT_TRUE(res->find("File: file2.txt") != std::string::npos);
  EXPECT_TRUE(res->find("Directory: subdir/") != std::string::npos);

  std::filesystem::remove_all("test_dir");
}

TEST_F(ContextManagementTest, ListDirectoryRecursive) {
  auto executor_or = ToolExecutor::Create(&db_);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::filesystem::create_directory("test_dir_rec");
  std::filesystem::create_directory("test_dir_rec/subdir");
  std::ofstream("test_dir_rec/subdir/subfile.txt") << "content";

  auto res = executor.Execute("list_directory", {{"path", "test_dir_rec"}, {"depth", 2}});
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(res->find("subdir/subfile.txt") != std::string::npos);

  std::filesystem::remove_all("test_dir_rec");
}

TEST_F(ContextManagementTest, ScratchpadBasic) {
  auto executor_or = ToolExecutor::Create(&db_);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;
  executor.SetSessionId("test-session");

  // Read empty scratchpad
  auto res_read1 = executor.Execute("manage_scratchpad", {{"action", "read"}});
  ASSERT_TRUE(res_read1.ok());
  EXPECT_TRUE(res_read1->find("Scratchpad is empty") != std::string::npos);

  // Update scratchpad
  auto res_update = executor.Execute("manage_scratchpad", {{"action", "update"}, {"content", "# My Notes\n- Task 1"}});
  ASSERT_TRUE(res_update.ok());

  // Read updated scratchpad
  auto res_read2 = executor.Execute("manage_scratchpad", {{"action", "read"}});
  ASSERT_TRUE(res_read2.ok());
  EXPECT_TRUE(res_read2->find("# My Notes") != std::string::npos);
  EXPECT_TRUE(res_read2->find("- Task 1") != std::string::npos);

  // Append to scratchpad
  auto res_append = executor.Execute("manage_scratchpad", {{"action", "append"}, {"content", "\n- Task 2"}});
  ASSERT_TRUE(res_append.ok());

  // Read appended scratchpad
  auto res_read3 = executor.Execute("manage_scratchpad", {{"action", "read"}});
  ASSERT_TRUE(res_read3.ok());
  EXPECT_TRUE(res_read3->find("- Task 1") != std::string::npos);
  EXPECT_TRUE(res_read3->find("- Task 2") != std::string::npos);
}

TEST_F(ContextManagementTest, DescribeDb) {
  auto executor_or = ToolExecutor::Create(&db_);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("describe_db", {});
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(res->find("\"name\":\"messages\"") != std::string::npos);
  EXPECT_TRUE(res->find("\"name\":\"tools\"") != std::string::npos);
}

TEST_F(ContextManagementTest, ReadFileWarning) {
  auto executor_or = ToolExecutor::Create(&db_);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::ofstream f("large_file.txt");
  for (int i = 0; i < 150; ++i) {
    f << "Line " << i << "\n";
  }
  f.close();

  // Read whole file
  auto res1 = executor.Execute("read_file", {{"path", "large_file.txt"}});
  ASSERT_TRUE(res1.ok());
  EXPECT_TRUE(res1->find("[NOTICE: This is a large file") != std::string::npos);

  // Read with range
  auto res2 = executor.Execute("read_file", {{"path", "large_file.txt"}, {"start_line", 1}, {"end_line", 10}});
  ASSERT_TRUE(res2.ok());
  EXPECT_TRUE(res2->find("[NOTICE:") == std::string::npos);

  std::filesystem::remove("large_file.txt");
}

TEST_F(ContextManagementTest, GrepTruncation) {
  auto executor_or = ToolExecutor::Create(&db_);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::ofstream f("large_grep.txt");
  for (int i = 0; i < 100; ++i) {
    f << "match " << i << "\n";
  }
  f.close();

  auto res = executor.Execute("grep_tool", {{"pattern", "match"}, {"path", "large_grep.txt"}});
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(res->find("[TRUNCATED") != std::string::npos);

  std::filesystem::remove("large_grep.txt");
}

}  // namespace slop
