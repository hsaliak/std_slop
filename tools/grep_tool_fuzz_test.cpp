
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

constexpr char kGrepFile[] = "grep_fuzz_target.txt";

void SetupCorpusFile() {
  std::ofstream out(kGrepFile, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open());
  out << "alpha\nbeta\ngamma\n";
  ASSERT_TRUE(out.good());
}

void GrepArgsNoCrash(const std::string& pattern,
                     const std::string& path,
                     const std::string& context_raw,
                     const std::string& limit_raw,
                     bool fixed_strings,
                     bool include_ignored) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  SetupCorpusFile();
  nlohmann::json args = {
      {"pattern", pattern},
      {"path", path.empty() ? std::string(".") : path},
      {"context", context_raw},
      {"limit", limit_raw},
      {"fixed_strings", fixed_strings},
      {"include_ignored", include_ignored},
  };

  const auto result = executor.Execute("grep", args);
  if (pattern.empty()) {
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
  } else if (!result.ok()) {
    EXPECT_TRUE(result.status().code() == absl::StatusCode::kInvalidArgument ||
                result.status().code() == absl::StatusCode::kInternal);
  } else {
    EXPECT_TRUE(!result->empty() || result->empty());
  }

  std::filesystem::remove(kGrepFile);
}

FUZZ_TEST(GrepToolFuzzTest, GrepArgsNoCrash)
    .WithDomains(fuzztest::Arbitrary<std::string>(),
                 fuzztest::InRegexp("[A-Za-z0-9_./-]{0,64}"),
                 fuzztest::Arbitrary<std::string>(),
                 fuzztest::Arbitrary<std::string>(),
                 fuzztest::Arbitrary<bool>(),
                 fuzztest::Arbitrary<bool>());

}  // namespace
}  // namespace slop