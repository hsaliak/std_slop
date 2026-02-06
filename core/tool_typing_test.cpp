#include <fstream>

#include "nlohmann/json.hpp"

#include "core/database.h"
#include "core/tool_executor.h"

#include <gtest/gtest.h>

namespace slop {

class ToolTypingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(db_.Init(":memory:").ok());
    auto executor_or = ToolExecutor::Create(&db_);
    ASSERT_TRUE(executor_or.ok());
    executor_ = std::move(*executor_or);
  }

  Database db_;
  std::unique_ptr<ToolExecutor> executor_;
};

TEST_F(ToolTypingTest, InvalidTypeHandling) {
  // read_file expects 'start_line' to be an int, pass a string
  auto res = executor_->Execute("read_file", {{"path", "test.txt"}, {"start_line", "invalid"}});
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(res->find("Error: INVALID_ARGUMENT") != std::string::npos);
  EXPECT_TRUE(res->find("must be an integer") != std::string::npos);
}

TEST_F(ToolTypingTest, MissingMandatoryField) {
  // write_file expects 'path' and 'content'
  auto res = executor_->Execute("write_file", {{"path", "test.txt"}});
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(res->find("Error: INVALID_ARGUMENT") != std::string::npos);
  EXPECT_TRUE(res->find("Missing mandatory field") != std::string::npos);
}

TEST_F(ToolTypingTest, DefaultValues) {
  // list_directory depth defaults to 1 (in the implementation logic)
  std::ofstream ofs("test_dir_file.txt");
  ofs << "test";
  ofs.close();

  auto res = executor_->Execute("list_directory", {{"path", "."}});
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(res->find("test_dir_file.txt") != std::string::npos);

  std::filesystem::remove("test_dir_file.txt");
}

TEST_F(ToolTypingTest, OptionalHandling) {
  // read_file start_line and end_line are optional
  std::ofstream ofs("test_optional.txt");
  for (int i = 1; i <= 10; ++i) ofs << "Line " << i << "\n";
  ofs.close();

  // No optional fields
  auto res1 = executor_->Execute("read_file", {{"path", "test_optional.txt"}});
  ASSERT_TRUE(res1.ok());
  EXPECT_TRUE(res1->find("RANGE: 1-10") != std::string::npos);

  // Partial optional fields
  auto res2 = executor_->Execute("read_file", {{"path", "test_optional.txt"}, {"start_line", 5}});
  ASSERT_TRUE(res2.ok());
  EXPECT_TRUE(res2->find("RANGE: 5-10") != std::string::npos);

  // Explicit null for optional fields
  auto res3 = executor_->Execute("read_file", {{"path", "test_optional.txt"}, {"end_line", nullptr}});
  ASSERT_TRUE(res3.ok());
  EXPECT_TRUE(res3->find("RANGE: 1-10") != std::string::npos);

  std::filesystem::remove("test_optional.txt");
}

TEST_F(ToolTypingTest, GitGrepFlexiblePath) {
  nlohmann::json args_str = {{"pattern", "foo"}, {"path", "core"}};
  nlohmann::json args_arr = {{"pattern", "foo"}, {"path", {"core", "interface"}}};

  auto res_git = executor_->Execute("execute_bash", {{"command", "git rev-parse --is-inside-work-tree"}});
  if (res_git.ok() && res_git->find("true") != std::string::npos) {
    auto res1 = executor_->Execute("git_grep_tool", args_str);
    EXPECT_TRUE(res1.ok());
    EXPECT_TRUE(res1->find("Error: INVALID_ARGUMENT") == std::string::npos);

    auto res2 = executor_->Execute("git_grep_tool", args_arr);
    EXPECT_TRUE(res2.ok());
    EXPECT_TRUE(res2->find("Error: INVALID_ARGUMENT") == std::string::npos);
  }
}

TEST_F(ToolTypingTest, ApplyPatchTyped) {
  nlohmann::json args = {{"path", "test_patch.txt"}, {"patches", {{{"find", "old"}, {"replace", "new"}}}}};

  std::ofstream ofs("test_patch.txt");
  ofs << "old content";
  ofs.close();

  auto res = executor_->Execute("apply_patch", args);
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(res->find("Successfully applied") != std::string::npos);

  std::ifstream ifs("test_patch.txt");
  std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  EXPECT_TRUE(content.find("new content") != std::string::npos);

  std::filesystem::remove("test_patch.txt");
}

}  // namespace slop
