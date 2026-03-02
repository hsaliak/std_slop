#include <filesystem>
#include <fstream>

#include "absl/strings/match.h"
#include "nlohmann/json.hpp"

#include "core/tool_executor.h"

#include <gtest/gtest.h>

namespace slop {
namespace {

nlohmann::json ParseEnvelope(const std::string& raw) {
  auto parsed = nlohmann::json::parse(raw, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
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

class ContextManagementTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(db_.Init(":memory:").ok());
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
  const std::string output = EnvelopeResultText(*res);
  EXPECT_TRUE(output.find("File: file1.txt") != std::string::npos);
  EXPECT_TRUE(output.find("File: file2.txt") != std::string::npos);
  EXPECT_TRUE(output.find("Directory: subdir/") != std::string::npos);

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
  EXPECT_TRUE(EnvelopeResultText(*res).find("subdir/subfile.txt") != std::string::npos);

  std::filesystem::remove_all("test_dir_rec");
}


TEST_F(ContextManagementTest, DescribeDb) {
  auto executor_or = ToolExecutor::Create(&db_);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  auto res = executor.Execute("describe_db", {});
  ASSERT_TRUE(res.ok());
  const std::string output = EnvelopeResultText(*res);
  EXPECT_TRUE(output.find("\"name\":\"messages\"") != std::string::npos);
  EXPECT_TRUE(output.find("\"name\":\"tools\"") != std::string::npos);
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
  EXPECT_TRUE(!EnvelopeResultText(*res1).empty());

  // Read with range
  auto res2 = executor.Execute("read_file", {{"path", "large_file.txt"}, {"start_line", 1}, {"end_line", 10}});
  ASSERT_TRUE(res2.ok());
  EXPECT_TRUE(!EnvelopeResultText(*res2).empty());

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

  auto res = executor.Execute("grep_tool", {{"pattern", "match"}, {"path", "large_grep.txt"}, {"limit", 50}});
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(EnvelopeResultText(*res).find("[TRUNCATED") != std::string::npos);

  std::filesystem::remove("large_grep.txt");
}

}  // namespace slop




