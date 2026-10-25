#include "tools/file_operations.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/str_split.h"

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop::file_ops {
namespace {

std::vector<std::string> SplitTestLines(const std::string& content) {
  std::vector<std::string> lines;
  std::istringstream input(content);
  std::string line;
  while (std::getline(input, line)) lines.push_back(line);
  return lines;
}

void FileLineRangeRespectsBounds(const std::string& content, const std::optional<int>& start_line,
                                 const std::optional<int>& end_line, bool line_numbers) {
  const std::vector<std::string> lines = SplitTestLines(content);
  const bool invalid = (start_line && *start_line < 1) || (end_line && *end_line < 1) ||
                       (start_line && end_line && *start_line > *end_line) ||
                       (start_line && static_cast<std::size_t>(*start_line) > lines.size());
  const auto result = FormatFileLines(content, {start_line, end_line, line_numbers});
  EXPECT_EQ(result.ok(), !invalid);
  if (!result.ok()) {
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    return;
  }

  EXPECT_TRUE(result->empty() || result->back() == '\n');
  const std::size_t actual_lines = static_cast<std::size_t>(std::count(result->begin(), result->end(), '\n'));
  const std::size_t first = static_cast<std::size_t>(start_line.value_or(1));
  const std::size_t last = std::min(lines.size(), static_cast<std::size_t>(end_line.value_or(
                                                              static_cast<int>(lines.size()))));
  const std::size_t expected_lines = lines.empty() || first > last ? 0 : last - first + 1;
  EXPECT_EQ(actual_lines, expected_lines);
}

void GrepMatchingReturnsOnlyMatchingLineNumbers(const std::string& content, const std::string& pattern,
                                               bool fixed_strings) {
  const auto matches = FindMatchingLines(content, pattern, fixed_strings);
  if (!matches.ok()) {
    EXPECT_FALSE(matches.status().message().empty());
    return;
  }
  const std::vector<std::string> lines = SplitTestLines(content);
  if (content.find('\0') != std::string::npos) {
    EXPECT_TRUE(matches->empty());
    return;
  }

  for (std::size_t index = 0; index < matches->size(); ++index) {
    const std::size_t line_number = (*matches)[index];
    ASSERT_GT(line_number, 0);
    ASSERT_LE(line_number, lines.size());
    if (index > 0) EXPECT_LT((*matches)[index - 1], line_number);
    if (fixed_strings) EXPECT_NE(lines[line_number - 1].find(pattern), std::string::npos);
  }
}

void WorkspacePathParserEnforcesRootRelativeComponents(const std::string& path) {
  const auto components = ParseWorkspaceRelativePath(path, true);
  bool has_parent_component = false;
  for (absl::string_view component : absl::StrSplit(path, '/')) {
    if (component == "..") has_parent_component = true;
  }
  const bool is_windows_absolute = path.size() >= 2 &&
                                   ((absl::ascii_isalpha(path[0]) && path[1] == ':') ||
                                    (path[0] == '\\' && path[1] == '\\'));
  const bool should_reject = path.empty() || path.size() > 4096 || path.find('\0') != std::string::npos ||
                             path.front() == '/' || path.front() == '\\' || is_windows_absolute ||
                             has_parent_component;
  EXPECT_EQ(components.ok(), !should_reject);
  if (!components.ok()) return;
  for (const std::string& component : *components) {
    EXPECT_FALSE(component.empty());
    EXPECT_NE(component, ".");
    EXPECT_NE(component, "..");
    EXPECT_EQ(component.find('/'), std::string::npos);
    EXPECT_EQ(component.find('\0'), std::string::npos);
  }
}

void IgnorePatternValidationEnforcesCountAndNulRules(const std::vector<std::string>& patterns) {
  const absl::Status status = ValidateIgnorePatterns(patterns);
  const bool expected_valid = patterns.size() <= 100 &&
                              std::all_of(patterns.begin(), patterns.end(), [](const std::string& pattern) {
                                return pattern.find('\0') == std::string::npos;
                              });
  EXPECT_EQ(status.ok(), expected_valid);
  if (!status.ok()) {
    EXPECT_EQ(status.code(), patterns.size() > 100 ? absl::StatusCode::kResourceExhausted
                                                   : absl::StatusCode::kInvalidArgument);
  }
}

FUZZ_TEST(WorkspaceFileOperationsFuzzTest, FileLineRangeRespectsBounds)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::optional<int>>(),
                 fuzztest::Arbitrary<std::optional<int>>(), fuzztest::Arbitrary<bool>());

FUZZ_TEST(WorkspaceFileOperationsFuzzTest, GrepMatchingReturnsOnlyMatchingLineNumbers)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>(),
                 fuzztest::Arbitrary<bool>());

FUZZ_TEST(WorkspaceFileOperationsFuzzTest, WorkspacePathParserEnforcesRootRelativeComponents)
    .WithDomains(fuzztest::Arbitrary<std::string>());

FUZZ_TEST(WorkspaceFileOperationsFuzzTest, IgnorePatternValidationEnforcesCountAndNulRules)
    .WithDomains(fuzztest::VectorOf(fuzztest::Arbitrary<std::string>()));

}  // namespace
}  // namespace slop::file_ops
