
#include "tools/common.h"

#include <string>

#include "absl/status/status.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace slop {
namespace {

void ValidateReadFileArgsDeterministic(const std::string& raw_args) {
  nlohmann::json args = nlohmann::json::parse(raw_args, nullptr, false);
  if (args.is_discarded()) {
    args = nlohmann::json::object();
  }

  const absl::Status first = ValidateReadFileArgs(args);
  const absl::Status second = ValidateReadFileArgs(args);
  EXPECT_EQ(first.ok(), second.ok());
  EXPECT_EQ(first.code(), second.code());
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
  if (path.empty()) {
    EXPECT_FALSE(status.ok());
    return;
  }
  if (path.find("..") != std::string::npos || path[0] == '/') {
    EXPECT_FALSE(status.ok());
    return;
  }
  if (start_line > end_line) {
    EXPECT_FALSE(status.ok());
    return;
  }
  EXPECT_TRUE(status.ok());
}

FUZZ_TEST(FileToolArgsFuzzTest, ValidateReadFileArgsDeterministic);

FUZZ_TEST(FileToolArgsFuzzTest, ReadFileRejectsTraversalOrAbsolute)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<bool>());

FUZZ_TEST(FileToolArgsFuzzTest, ReadFileRangeValidation)
    .WithDomains(fuzztest::InRegexp("[A-Za-z0-9_./-]{0,64}"), fuzztest::Arbitrary<int>(), fuzztest::Arbitrary<int>());

}  // namespace
}  // namespace slop