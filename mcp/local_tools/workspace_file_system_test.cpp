#include "mcp/local_tools/workspace_file_system.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/match.h"
#include "absl/time/time.h"

#include <gtest/gtest.h>

namespace slop::mcp::local_tools {
namespace {

std::filesystem::path MakeTempDirectory() {
  const std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / absl::StrCat("workspace_fs_", absl::ToUnixNanos(absl::Now()));
  std::error_code error;
  std::filesystem::create_directories(path, error);
  EXPECT_FALSE(error) << error.message();
  return path;
}

void WriteTestFile(const std::filesystem::path& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open()) << path.string();
  output << content;
  ASSERT_TRUE(output.good()) << path.string();
}

std::string ReadTestFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  EXPECT_TRUE(input.is_open()) << path.string();
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

TEST(WorkspaceFileSystemTest, ParsesRelativePathsAndRejectsTraversalAndAbsoluteForms) {
  auto parsed = ParseWorkspaceRelativePath("./src//file.cpp", false);
  ASSERT_TRUE(parsed.ok()) << parsed.status().message();
  EXPECT_EQ(*parsed, (std::vector<std::string>{"src", "file.cpp"}));
  EXPECT_FALSE(ParseWorkspaceRelativePath(".", false).ok());
  EXPECT_TRUE(ParseWorkspaceRelativePath(".", true)->empty());
  EXPECT_FALSE(ParseWorkspaceRelativePath("../outside", false).ok());
  EXPECT_FALSE(ParseWorkspaceRelativePath("sub/../outside", false).ok());
  EXPECT_FALSE(ParseWorkspaceRelativePath("/outside", false).ok());
  EXPECT_FALSE(ParseWorkspaceRelativePath("C:/outside", false).ok());
  EXPECT_FALSE(ParseWorkspaceRelativePath("\\\\server\\share", false).ok());
  EXPECT_FALSE(ParseWorkspaceRelativePath(std::string("file\0name", 9), false).ok());
  EXPECT_TRUE(ParseWorkspaceRelativePath("name..part/file", false).ok());
}

TEST(WorkspaceFileSystemTest, FindsLiteralAndRegexLinesInProcess) {
  auto literal = FindMatchingLines("before\nneedle\nother needle\n", "needle", true);
  ASSERT_TRUE(literal.ok()) << literal.status().message();
  EXPECT_EQ(*literal, (std::vector<std::size_t>{2, 3}));

  auto regex = FindMatchingLines("match\nother\nmatch again\n", "^match", false);
  ASSERT_TRUE(regex.ok()) << regex.status().message();
  EXPECT_EQ(*regex, (std::vector<std::size_t>{1, 3}));
  const auto invalid = FindMatchingLines("text\n", "[", false);
  ASSERT_FALSE(invalid.ok());
  EXPECT_EQ(invalid.status().code(), absl::StatusCode::kInvalidArgument);
  const auto oversized = FindMatchingLines("text\n", std::string(4097, 'a'), false);
  ASSERT_FALSE(oversized.ok());
  EXPECT_EQ(oversized.status().code(), absl::StatusCode::kResourceExhausted);

  const std::string binary_content("text\0content", 12);
  auto binary_regex = FindMatchingLines(binary_content, "[", false);
  ASSERT_FALSE(binary_regex.ok());
  EXPECT_EQ(binary_regex.status().code(), absl::StatusCode::kInvalidArgument);
  auto binary_literal = FindMatchingLines(binary_content, "text", true);
  ASSERT_TRUE(binary_literal.ok()) << binary_literal.status().message();
  EXPECT_TRUE(binary_literal->empty());
}

TEST(WorkspaceFileSystemTest, ReadsRangesAndNumbersLines) {
  const std::filesystem::path root = MakeTempDirectory();
  ASSERT_FALSE(root.empty());
  absl::Cleanup remove_root = [&root] {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  };
  WriteTestFile(root / "lines.txt", "one\ntwo\nthree\n");

  auto content = ReadWorkspaceFile(root, "lines.txt", ReadFileOptions{2, 3, false});
  ASSERT_TRUE(content.ok()) << content.status().message();
  EXPECT_EQ(*content, "two\nthree\n");

  auto numbered = ReadWorkspaceFile(root, "lines.txt", ReadFileOptions{1, 2, true});
  ASSERT_TRUE(numbered.ok()) << numbered.status().message();
  EXPECT_EQ(*numbered, "1: one\n2: two\n");
  EXPECT_FALSE(ReadWorkspaceFile(root, "lines.txt", ReadFileOptions{5, std::nullopt, false}).ok());
}

TEST(WorkspaceFileSystemTest, ReadsAndWritesDoNotFollowSymlinks) {
  const std::filesystem::path root = MakeTempDirectory();
  ASSERT_FALSE(root.empty());
  absl::Cleanup remove_root = [&root] {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  };
  const std::filesystem::path outside = root.parent_path() / absl::StrCat(root.filename().string(), "_outside");
  std::error_code error;
  std::filesystem::create_directories(outside, error);
  ASSERT_FALSE(error) << error.message();
  absl::Cleanup remove_outside = [&outside] {
    std::error_code cleanup_error;
    std::filesystem::remove_all(outside, cleanup_error);
  };
  WriteTestFile(outside / "secret.txt", "private");
  std::filesystem::create_directory_symlink(outside, root / "escape", error);
  ASSERT_FALSE(error) << error.message();
  std::filesystem::create_symlink(outside / "secret.txt", root / "secret_link.txt", error);
  ASSERT_FALSE(error) << error.message();

  auto escaped_cwd = ResolveWorkspaceDirectory(root, "escape");
  ASSERT_FALSE(escaped_cwd.ok());
  EXPECT_EQ(escaped_cwd.status().code(), absl::StatusCode::kPermissionDenied);
  auto escaped_read = ReadWorkspaceFile(root, "escape/secret.txt", ReadFileOptions{});
  ASSERT_FALSE(escaped_read.ok());
  EXPECT_EQ(escaped_read.status().code(), absl::StatusCode::kPermissionDenied);
  auto linked_read = ReadWorkspaceFile(root, "secret_link.txt", ReadFileOptions{});
  ASSERT_FALSE(linked_read.ok());
  EXPECT_EQ(linked_read.status().code(), absl::StatusCode::kPermissionDenied);
  auto linked_write = WriteWorkspaceFile(root, "secret_link.txt", "changed");
  ASSERT_FALSE(linked_write.ok());
  EXPECT_EQ(linked_write.status().code(), absl::StatusCode::kPermissionDenied);
  EXPECT_EQ(ReadTestFile(outside / "secret.txt"), "private");

  auto listing = ListWorkspaceDirectory(root, ".", 4, true, {});
  ASSERT_TRUE(listing.ok()) << listing.status().message();
  EXPECT_TRUE(absl::StrContains(*listing, "Symlink: escape"));
  EXPECT_TRUE(absl::StrContains(*listing, "Symlink: secret_link.txt"));
  EXPECT_FALSE(absl::StrContains(*listing, "escape/secret.txt"));
}

TEST(WorkspaceFileSystemTest, ListsSymlinksButDoesNotTraverseThem) {
  const std::filesystem::path root = MakeTempDirectory();
  ASSERT_FALSE(root.empty());
  absl::Cleanup remove_root = [&root] {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  };
  std::filesystem::create_directories(root / "nested");
  WriteTestFile(root / "nested" / "file.txt", "needle\n");

  auto nested_directory = ResolveWorkspaceDirectory(root, "nested");
  ASSERT_TRUE(nested_directory.ok()) << nested_directory.status().message();
  EXPECT_EQ(*nested_directory, root / "nested");

  auto listing = ListWorkspaceDirectory(root, ".", 3, false, {});
  ASSERT_TRUE(listing.ok()) << listing.status().message();
  EXPECT_TRUE(absl::StrContains(*listing, "Directory: nested/"));
  EXPECT_TRUE(absl::StrContains(*listing, "File: nested/file.txt"));
}

TEST(WorkspaceFileSystemTest, GrepSupportsLiteralRegexContextAndFilePaths) {
  const std::filesystem::path root = MakeTempDirectory();
  ASSERT_FALSE(root.empty());
  absl::Cleanup remove_root = [&root] {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  };
  std::filesystem::create_directories(root / "src");
  WriteTestFile(root / "src" / "match.txt", "before\nneedle one\nafter\nmatch two\n");

  auto fixed = GrepWorkspaceFiles(root, "src", "needle", 1, 10, true, false, {});
  ASSERT_TRUE(fixed.ok()) << fixed.status().message();
  EXPECT_TRUE(absl::StrContains(*fixed, "src/match.txt:2:needle one"));
  EXPECT_TRUE(absl::StrContains(*fixed, "src/match.txt-1-before"));
  EXPECT_TRUE(absl::StrContains(*fixed, "src/match.txt-3-after"));

  auto limited = GrepWorkspaceFiles(root, "src", "before", 0, 1, true, false, {});
  ASSERT_TRUE(limited.ok()) << limited.status().message();
  EXPECT_EQ(*limited, "src/match.txt:1:before");

  auto regex = GrepWorkspaceFiles(root, "src/match.txt", "^match", 0, 10, false, false, {});
  ASSERT_TRUE(regex.ok()) << regex.status().message();
  EXPECT_TRUE(absl::StrContains(*regex, "src/match.txt:4:match two"));
  EXPECT_FALSE(GrepWorkspaceFiles(root, ".", "[", 0, 10, false, false, {}).ok());
}

TEST(WorkspaceFileSystemTest, EditsAreValidatedBeforeAtomicReplacement) {
  const std::filesystem::path root = MakeTempDirectory();
  ASSERT_FALSE(root.empty());
  absl::Cleanup remove_root = [&root] {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  };
  WriteTestFile(root / "edit.txt", "alpha beta beta\n");

  const nlohmann::json ambiguous = {{{"op", "replace"}, {"find", "beta"}, {"text", "BETA"}}};
  auto failed = EditWorkspaceFile(root, "edit.txt", ambiguous);
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(ReadTestFile(root / "edit.txt"), "alpha beta beta\n");

  const nlohmann::json edits = {
      {{"op", "replace"}, {"find", "beta"}, {"text", "BETA"}, {"which", "first"}},
  };
  auto changed = EditWorkspaceFile(root, "edit.txt", edits);
  ASSERT_TRUE(changed.ok()) << changed.status().message();
  EXPECT_EQ(changed->content, "alpha BETA beta\n");
  EXPECT_EQ(ReadTestFile(root / "edit.txt"), "alpha BETA beta\n");
}

}  // namespace
}  // namespace slop::mcp::local_tools
