
#include "core/json_utils.h"
#include "tools/common.h"
#include "tools/file_operations.h"

#include <string>

#include "absl/status/status.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace slop {
namespace {

void ReadFileArgumentValidationEnforcesPathAndRangeInvariants(const std::string& raw_args) {
  nlohmann::json args = nlohmann::json::parse(raw_args, nullptr, false);
  if (args.is_discarded()) args = nlohmann::json::object();

  const absl::Status status = ValidateReadFileArgs(args);
  if (!status.ok()) {
    EXPECT_FALSE(status.message().empty());
    return;
  }

  const auto path = json_get<std::string>(args, "path");
  ASSERT_TRUE(path.has_value());
  EXPECT_TRUE(file_ops::ParseWorkspaceRelativePath(*path, false).ok());
  auto start_line = ParseOptionalInteger(args, "start_line");
  ASSERT_TRUE(start_line.ok());
  auto end_line = ParseOptionalInteger(args, "end_line");
  ASSERT_TRUE(end_line.ok());
  EXPECT_FALSE(*start_line && **start_line < 1);
  EXPECT_FALSE(*end_line && **end_line < 1);
  if (*start_line && *end_line) EXPECT_LE(**start_line, **end_line);
}

void ReadFileRejectsTraversalOrAbsolute(const std::string& middle, bool absolute) {
  nlohmann::json args;
  args["path"] = absolute ? std::string("/tmp/") + middle : std::string("safe/../") + middle;

  const absl::Status status = ValidateReadFileArgs(args);
  EXPECT_FALSE(status.ok());
}

void ReadFileRangeValidation(const std::string& path, int start_line, int end_line) {
  nlohmann::json args;
  args["path"] = path;
  args["start_line"] = start_line;
  args["end_line"] = end_line;

  const absl::Status status = ValidateReadFileArgs(args);
  const bool expected_valid = file_ops::ParseWorkspaceRelativePath(path, false).ok() && start_line >= 1 &&
                              end_line >= 1 && start_line <= end_line;
  EXPECT_EQ(status.ok(), expected_valid);
  if (!status.ok()) EXPECT_FALSE(status.message().empty());
}

TEST(FileToolArgsTest, RejectsNonPositiveLineNumbers) {
  EXPECT_FALSE(ValidateReadFileArgs({{"path", "file.txt"}, {"start_line", 0}}).ok());
  EXPECT_FALSE(ValidateReadFileArgs({{"path", "file.txt"}, {"end_line", -1}}).ok());
}

FUZZ_TEST(FileToolArgsFuzzTest, ReadFileArgumentValidationEnforcesPathAndRangeInvariants);

FUZZ_TEST(FileToolArgsFuzzTest, ReadFileRejectsTraversalOrAbsolute)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<bool>());

FUZZ_TEST(FileToolArgsFuzzTest, ReadFileRangeValidation)
    .WithDomains(fuzztest::InRegexp("[A-Za-z0-9_./-]{0,64}"), fuzztest::Arbitrary<int>(), fuzztest::Arbitrary<int>());

}  // namespace
}  // namespace slop