
#include "tools/tool_executor.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

#include "core/database.h"
#include "nlohmann/json.hpp"

namespace slop {
namespace {

constexpr char kPatchTargetFile[] = "patch_tool_fuzz_target.txt";
constexpr char kBaselineContent[] = "alpha\nbeta\ngamma\n";

void WriteBaselineFile() {
  std::ofstream out(kPatchTargetFile, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open());
  out << kBaselineContent;
  ASSERT_TRUE(out.good());
}

std::string ReadWholeFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.is_open());
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void PatchToolArgShapeNoCrash(const std::string& path_raw,
                              const std::string& diff_raw,
                              int path_shape,
                              int diff_shape,
                              bool dry_run,
                              bool ignore_whitespace) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  nlohmann::json args = nlohmann::json::object();
  switch (path_shape & 3) {
    case 0:
      args["path"] = path_raw;
      break;
    case 1:
      args["path"] = 7;
      break;
    case 2:
      args["path"] = nullptr;
      break;
    default:
      break;
  }

  switch (diff_shape & 3) {
    case 0:
      args["unified_diff"] = diff_raw;
      break;
    case 1:
      args["unified_diff"] = true;
      break;
    case 2:
      args["unified_diff"] = nlohmann::json::array({"@@"});
      break;
    default:
      break;
  }

  args["dry_run"] = dry_run;
  args["ignore_whitespace"] = ignore_whitespace;

  const auto res = executor.Execute("patch_tool", args);
  if (!res.ok()) {
    return;
  }
  const auto parsed = nlohmann::json::parse(*res, nullptr, false);
  EXPECT_FALSE(parsed.is_discarded());
  if (parsed.is_object()) {
    EXPECT_TRUE(parsed.contains("ok"));
  }
}

void PatchToolDryRunNoWrite(const std::string& unified_diff, bool ignore_whitespace) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::filesystem::remove(kPatchTargetFile);
  WriteBaselineFile();
  const std::string before = ReadWholeFile(kPatchTargetFile);

  nlohmann::json args = {
      {"path", kPatchTargetFile},
      {"unified_diff", unified_diff},
      {"dry_run", true},
      {"ignore_whitespace", ignore_whitespace},
  };

  const auto res = executor.Execute("patch_tool", args);
  ASSERT_TRUE(res.ok()) << res.status().message();

  const auto parsed = nlohmann::json::parse(*res, nullptr, false);
  ASSERT_FALSE(parsed.is_discarded());
  ASSERT_TRUE(parsed.is_object());
  EXPECT_TRUE(parsed.contains("ok"));

  const std::string after = ReadWholeFile(kPatchTargetFile);
  EXPECT_EQ(before, after);

  std::filesystem::remove(kPatchTargetFile);
}

FUZZ_TEST(PatchToolFuzzTest, PatchToolArgShapeNoCrash);

FUZZ_TEST(PatchToolFuzzTest, PatchToolDryRunNoWrite)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<bool>());

}  // namespace
}  // namespace slop