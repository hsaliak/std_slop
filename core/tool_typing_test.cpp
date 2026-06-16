#include <fstream>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "nlohmann/json.hpp"

#include "core/database.h"
#include "tools/tool_executor.h"

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
  std::ofstream f("test.txt");
  f << "hello\n";
  f.close();
  // read_file expects 'start_line' to be an int, pass a string
  auto res = executor_->Execute("read_file", {{"path", "test.txt"}, {"start_line", "invalid"}});
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(res.status().message(), "must be an integer"));
}

TEST_F(ToolTypingTest, MissingMandatoryField) {
  // write_file expects 'path' and 'content'
  auto res = executor_->Execute("write_file", {{"path", "test.txt"}});
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(res.status().message(), "Missing mandatory field: content"));
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
  EXPECT_TRUE(!res1->empty());

  // Partial optional fields
  auto res2 = executor_->Execute("read_file", {{"path", "test_optional.txt"}, {"start_line", 5}});
  ASSERT_TRUE(res2.ok());
  EXPECT_TRUE(!res2->empty());

  // Numeric strings should be accepted for optional integer fields
  auto res_numeric_str =
      executor_->Execute("read_file", {{"path", "test_optional.txt"}, {"start_line", "5"}, {"end_line", "6"}});
  ASSERT_TRUE(res_numeric_str.ok());
  EXPECT_TRUE(res_numeric_str->find("Line 5") != std::string::npos);
  EXPECT_TRUE(res_numeric_str->find("Line 6") != std::string::npos);
  EXPECT_TRUE(res_numeric_str->find("Line 7") == std::string::npos);

  // Explicit null for optional fields
  auto res3 = executor_->Execute("read_file", {{"path", "test_optional.txt"}, {"end_line", nullptr}});
  ASSERT_TRUE(res3.ok());
  EXPECT_TRUE(!res3->empty());

  std::filesystem::remove("test_optional.txt");
}

TEST_F(ToolTypingTest, GrepDirectTypedArgs) {
  auto res_ok = executor_->Execute("grep", {{"pattern", "foo"}, {"path", "core"}});
  EXPECT_TRUE(res_ok.ok() || res_ok.status().code() == absl::StatusCode::kNotFound);
}

TEST_F(ToolTypingTest, EditToolTyped) {
  nlohmann::json args = {{"path", "test_edit.txt"},
                         {"edits", nlohmann::json::array({{{"op", "replace"}, {"find", "old content"}, {"text", "new content"}}})}};

  std::ofstream ofs("test_edit.txt");
  ofs << "old content\n";
  ofs.close();

  auto res = executor_->Execute("edit_tool", args);
  ASSERT_TRUE(res.ok()) << res.status().message();
  ASSERT_TRUE(absl::StrContains(*res, R"("edits":1)")) << *res;
  ASSERT_TRUE(absl::StrContains(*res, R"("bytes_before")"));

  std::ifstream ifs("test_edit.txt");
  std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  EXPECT_TRUE(absl::StrContains(content, "new content"));

  std::filesystem::remove("test_edit.txt");
}

}  // namespace slop
