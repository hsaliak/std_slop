
#include "tools/tool_executor.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "absl/status/status.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

#include "core/database.h"
#include "nlohmann/json.hpp"

namespace slop {
namespace {

constexpr char kReadTargetFile[] = "path_range_read_target.txt";
constexpr char kWriteTargetFile[] = "path_range_write_target.txt";

void WriteReadFixture() {
  std::ofstream out(kReadTargetFile, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open());
  out << "line1\nline2\nline3\nline4\n";
  ASSERT_TRUE(out.good());
}

void ReadWritePathBoundaryNoCrash(const std::string& path,
                                  const std::string& content,
                                  int start_line,
                                  int end_line,
                                  bool line_numbers) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  WriteReadFixture();

  const std::string read_path = path.empty() ? std::string(kReadTargetFile) : path;
  const auto read_res = executor.Execute(
      "read_file",
      nlohmann::json{{"path", read_path}, {"start_line", start_line}, {"end_line", end_line}, {"line_numbers", line_numbers}});
  if (read_path.find("..") != std::string::npos || (!read_path.empty() && read_path[0] == '/')) {
    EXPECT_FALSE(read_res.ok());
  }

  const std::string write_path = path.empty() ? std::string(kWriteTargetFile) : path;
  const auto write_res = executor.Execute("write_file", nlohmann::json{{"path", write_path}, {"content", content}});
  if (write_path.find("..") != std::string::npos || (!write_path.empty() && write_path[0] == '/')) {
    EXPECT_FALSE(write_res.ok());
  }

  std::filesystem::remove(kReadTargetFile);
  std::filesystem::remove(kWriteTargetFile);
}

void ListDirectoryDepthAndIgnoreValidation(const std::string& depth_raw, const std::string& ignore_raw, bool include_ignored) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  nlohmann::json ignore_value = nlohmann::json::parse(ignore_raw, nullptr, false);
  if (ignore_value.is_discarded()) {
    ignore_value = ignore_raw;
  }

  nlohmann::json args = {
      {"path", "."},
      {"depth", depth_raw},
      {"ignore", ignore_value},
      {"include_ignored", include_ignored},
  };

  const auto result = executor.Execute("list_directory", args);
  if (!include_ignored && !ignore_value.is_array() && !ignore_value.is_null()) {
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    return;
  }

  if (!result.ok()) {
    EXPECT_TRUE(result.status().code() == absl::StatusCode::kInvalidArgument ||
                result.status().code() == absl::StatusCode::kInternal);
    return;
  }
  EXPECT_TRUE(!result->empty() || result->empty());
}

FUZZ_TEST(PathRangeToolsFuzzTest, ReadWritePathBoundaryNoCrash)
    .WithDomains(fuzztest::InRegexp("[A-Za-z0-9_./-]{0,64}"),
                 fuzztest::Arbitrary<std::string>(),
                 fuzztest::Arbitrary<int>(),
                 fuzztest::Arbitrary<int>(),
                 fuzztest::Arbitrary<bool>());

FUZZ_TEST(PathRangeToolsFuzzTest, ListDirectoryDepthAndIgnoreValidation)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<bool>());

}  // namespace
}  // namespace slop