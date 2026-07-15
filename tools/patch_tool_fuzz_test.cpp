
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

nlohmann::json ParsePatchResult(const absl::StatusOr<std::string>& result) {
  EXPECT_TRUE(result.ok()) << result.status();
  return nlohmann::json::parse(*result, nullptr, false);
}

TEST(PatchToolTest, RejectsSymlinkOutsideRepository) {
  constexpr char kLinkPath[] = "patch_tool_outside_link";
  const std::filesystem::path outside_path = std::filesystem::temp_directory_path() / "patch_tool_outside_target.txt";
  {
    std::ofstream outside(outside_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(outside.is_open());
    outside << kBaselineContent;
  }
  std::error_code error;
  std::filesystem::remove(kLinkPath, error);
  std::filesystem::create_symlink(outside_path, kLinkPath, error);
  ASSERT_FALSE(error) << error.message();

  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  const auto result = (*executor_or)->Execute(
      "patch_tool", {{"path", kLinkPath}, {"unified_diff", "@@ -1 +1 @@\n-alpha\n+changed\n"}, {"dry_run", true}});

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kPermissionDenied);
  EXPECT_EQ(ReadWholeFile(outside_path.string()), kBaselineContent);
  std::filesystem::remove(kLinkPath, error);
  std::filesystem::remove(outside_path, error);
}

TEST(PatchToolTest, HonorsIgnoreWhitespaceOption) {
  WriteBaselineFile();
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  const nlohmann::json diff = "@@ -1 +1 @@\n-alpha  \n+changed\n";

  const nlohmann::json strict = ParsePatchResult((*executor_or)->Execute(
      "patch_tool", {{"path", kPatchTargetFile}, {"unified_diff", diff}, {"dry_run", true}, {"ignore_whitespace", false}}));
  ASSERT_TRUE(strict.is_object());
  EXPECT_FALSE(strict.at("ok"));

  const nlohmann::json relaxed = ParsePatchResult((*executor_or)->Execute(
      "patch_tool", {{"path", kPatchTargetFile}, {"unified_diff", diff}, {"dry_run", true}, {"ignore_whitespace", true}}));
  ASSERT_TRUE(relaxed.is_object());
  EXPECT_TRUE(relaxed.at("ok"));
  std::filesystem::remove(kPatchTargetFile);
}

TEST(PatchToolTest, AddsToEmptyFileWithoutLeadingNewline) {
  std::ofstream out(kPatchTargetFile, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open());
  out.close();

  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  const nlohmann::json result = ParsePatchResult((*executor_or)->Execute(
      "patch_tool", {{"path", kPatchTargetFile}, {"unified_diff", "@@ -0,0 +1 @@\n+content\n"}}));

  ASSERT_TRUE(result.is_object());
  EXPECT_TRUE(result.at("ok"));
  EXPECT_EQ(ReadWholeFile(kPatchTargetFile), "content");
  std::filesystem::remove(kPatchTargetFile);
}

TEST(PatchToolTest, AppliesNoNewlineAtEndOfFileMarker) {
  WriteBaselineFile();
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  const nlohmann::json result = ParsePatchResult((*executor_or)->Execute(
      "patch_tool", {{"path", kPatchTargetFile},
                     {"unified_diff", "@@ -3 +3 @@\n-gamma\n+changed\n\\ No newline at end of file\n"}}));

  ASSERT_TRUE(result.is_object());
  EXPECT_TRUE(result.at("ok"));
  EXPECT_EQ(ReadWholeFile(kPatchTargetFile), "alpha\nbeta\nchanged");
  std::filesystem::remove(kPatchTargetFile);
}

TEST(PatchToolTest, RejectsMalformedDiffWithoutWriting) {
  WriteBaselineFile();
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());

  for (const std::string& diff : {"@@ malformed @@\n-alpha\n+changed\n", "@@ -1 +1 @@\n-alpha\ninvalid\n+changed\n",
                                  "@@ -1,2 +1,2 @@\n alpha\n"}) {
    const nlohmann::json result = ParsePatchResult(
        (*executor_or)->Execute("patch_tool", {{"path", kPatchTargetFile}, {"unified_diff", diff}}));
    ASSERT_TRUE(result.is_object());
    EXPECT_FALSE(result.at("ok"));
    EXPECT_EQ(result.at("code"), "PATCH_PARSE_FAILED");
    EXPECT_EQ(ReadWholeFile(kPatchTargetFile), kBaselineContent);
  }
  std::filesystem::remove(kPatchTargetFile);
}

TEST(PatchToolTest, RequiresStagingBranchInMailMode) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  (*executor_or)->SetMailMode(true);

  setenv("SLOP_FORCE_BRANCH_NAME", "main", 1);
  unsetenv("SLOP_SKIP_STAGING_CHECK");
  const auto result = (*executor_or)->Execute(
      "patch_tool", {{"path", kPatchTargetFile}, {"unified_diff", "@@ -1 +1 @@\n-alpha\n+changed\n"}});
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kFailedPrecondition);

  unsetenv("SLOP_FORCE_BRANCH_NAME");
  setenv("SLOP_SKIP_STAGING_CHECK", "1", 1);
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