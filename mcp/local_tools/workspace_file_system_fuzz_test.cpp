#include "mcp/local_tools/workspace_file_system.h"

#include <optional>
#include <string>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop::mcp::local_tools {
namespace {

void FileLineRangeIsDeterministic(const std::string& content, const std::optional<int>& start_line,
                                  const std::optional<int>& end_line, bool line_numbers) {
  const ReadFileOptions options{start_line, end_line, line_numbers};
  const auto first = FormatFileLines(content, options);
  const auto second = FormatFileLines(content, options);
  EXPECT_EQ(first.ok(), second.ok());
  if (!first.ok()) {
    EXPECT_EQ(first.status().code(), second.status().code());
    return;
  }
  EXPECT_EQ(*first, *second);
  EXPECT_TRUE(first->empty() || first->back() == '\n');
}

void GrepLiteralPatternNeverCrashes(const std::string& pattern) {
  const auto result = GrepWorkspaceFiles("/dev/null", ".", pattern, 0, 1, true, false, {});
  if (!result.ok()) EXPECT_FALSE(result.status().message().empty());
}

void GrepRegexPatternNeverCrashes(const std::string& pattern) {
  const auto result = GrepWorkspaceFiles("/dev/null", ".", pattern, 0, 1, false, false, {});
  if (!result.ok()) EXPECT_FALSE(result.status().message().empty());
}

void WorkspacePathParserIsDeterministic(const std::string& path) {
  const auto first = ParseWorkspaceRelativePath(path, true);
  const auto second = ParseWorkspaceRelativePath(path, true);
  EXPECT_EQ(first.ok(), second.ok());
  if (!first.ok()) {
    EXPECT_EQ(first.status().code(), second.status().code());
    return;
  }

  EXPECT_EQ(*first, *second);
  for (const std::string& component : *first) {
    EXPECT_FALSE(component.empty());
    EXPECT_NE(component, ".");
    EXPECT_NE(component, "..");
    EXPECT_EQ(component.find('/'), std::string::npos);
    EXPECT_EQ(component.find('\0'), std::string::npos);
  }
}

FUZZ_TEST(WorkspaceFileSystemFuzzTest, FileLineRangeIsDeterministic)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::optional<int>>(),
                 fuzztest::Arbitrary<std::optional<int>>(), fuzztest::Arbitrary<bool>());

FUZZ_TEST(WorkspaceFileSystemFuzzTest, GrepLiteralPatternNeverCrashes)
    .WithDomains(fuzztest::Arbitrary<std::string>());

FUZZ_TEST(WorkspaceFileSystemFuzzTest, GrepRegexPatternNeverCrashes)
    .WithDomains(fuzztest::Arbitrary<std::string>());

FUZZ_TEST(WorkspaceFileSystemFuzzTest, WorkspacePathParserIsDeterministic)
    .WithDomains(fuzztest::Arbitrary<std::string>());

}  // namespace
}  // namespace slop::mcp::local_tools
