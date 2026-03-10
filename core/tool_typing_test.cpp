#include <fstream>

#include "absl/status/status.h"
#include "absl/strings/match.h"
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

  auto res = executor_->Execute("run_js", {{"script", R"(
    return tools.list_directory({path: '.'});
  )"}});
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

TEST_F(ToolTypingTest, GrepFlexiblePath) {
  nlohmann::json args_str = {{"pattern", "foo"}, {"path", "core"}};

  auto res_git = executor_->Execute("execute_bash", {{"command", "git rev-parse --is-inside-work-tree"}});
  if (res_git.ok() && res_git->find("true") != std::string::npos) {
    auto res1 = executor_->Execute("run_js", {{"script", R"(
      return tools.grep_tool(args.payload);
    )"}, {"args", {{"payload", args_str}}}});
    EXPECT_TRUE(res1.ok());
    EXPECT_TRUE(res1->find("Error: INVALID_ARGUMENT") == std::string::npos);

    // Path arrays are no longer supported in the simplified interface.
  }
}

TEST_F(ToolTypingTest, GitGrepTypedArgs) {
  auto res_ok = executor_->Execute("run_js", {{"script", R"(
    return tools.git_grep({pattern: 'foo', paths: ['core']});
  )"}});
  ASSERT_TRUE(res_ok.ok());
  EXPECT_TRUE(res_ok->find("INVALID_ARGUMENT") == std::string::npos);

  auto res_bad = executor_->Execute("run_js", {{"script", R"(
    return tools.git_grep({pattern: 'foo', paths: 'core'});
  )"}});
  ASSERT_FALSE(res_bad.ok());
  EXPECT_TRUE(absl::StrContains(res_bad.status().message(), "paths must be an array of strings"));
}

TEST_F(ToolTypingTest, ApplyPatchTyped) {
  std::string unified_diff =
      "--- test_patch.txt\n"
      "+++ test_patch.txt\n"
      "@@ -1,1 +1,1 @@\n"
      "-old content\n"
      "+new content\n";
  nlohmann::json args = {{"path", "test_patch.txt"}, {"unified_diff", unified_diff}};

  std::ofstream ofs("test_patch.txt");
  ofs << "old content\n";
  ofs.close();

  auto res = executor_->Execute("patch_tool", args);
  ASSERT_TRUE(res.ok());
  ASSERT_TRUE(absl::StrContains(*res, R"("ok":true)")) << *res;
  ASSERT_TRUE(absl::StrContains(*res, R"("mode":"apply")"));
  ASSERT_TRUE(absl::StrContains(*res, R"("applied":1)"));

  std::ifstream ifs("test_patch.txt");
  std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  EXPECT_TRUE(absl::StrContains(content, "new content"));

  std::filesystem::remove("test_patch.txt");
}

}  // namespace slop



