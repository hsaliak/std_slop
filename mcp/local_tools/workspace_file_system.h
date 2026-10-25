#ifndef SLOP_MCP_LOCAL_TOOLS_WORKSPACE_FILE_SYSTEM_H_
#define SLOP_MCP_LOCAL_TOOLS_WORKSPACE_FILE_SYSTEM_H_

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "nlohmann/json.hpp"

#include "tools/edit_engine.h"

namespace slop::mcp::local_tools {

struct ReadFileOptions {
  std::optional<int> start_line;
  std::optional<int> end_line;
  bool line_numbers = false;
};

absl::StatusOr<std::string> FormatFileLines(absl::string_view content, const ReadFileOptions& options);

absl::StatusOr<std::vector<std::string>> ParseWorkspaceRelativePath(absl::string_view path, bool allow_root);
absl::StatusOr<std::vector<std::size_t>> FindMatchingLines(absl::string_view content, absl::string_view pattern,
                                                          bool fixed_strings);
absl::StatusOr<std::filesystem::path> ResolveWorkspaceDirectory(const std::filesystem::path& root,
                                                                absl::string_view path);
absl::StatusOr<std::string> ReadWorkspaceFile(const std::filesystem::path& root, absl::string_view path,
                                              const ReadFileOptions& options);
absl::StatusOr<std::string> ListWorkspaceDirectory(const std::filesystem::path& root, absl::string_view path,
                                                   int depth, bool include_ignored,
                                                   const std::vector<std::string>& ignore_patterns);
absl::StatusOr<std::string> GrepWorkspaceFiles(const std::filesystem::path& root, absl::string_view path,
                                               absl::string_view pattern, int context, int limit, bool fixed_strings,
                                               bool include_ignored,
                                               const std::vector<std::string>& ignore_patterns);
absl::StatusOr<std::string> WriteWorkspaceFile(const std::filesystem::path& root, absl::string_view path,
                                               absl::string_view content);
absl::StatusOr<slop::TextEditResult> EditWorkspaceFile(const std::filesystem::path& root, absl::string_view path,
                                                       const nlohmann::json& edits);

}  // namespace slop::mcp::local_tools

#endif  // SLOP_MCP_LOCAL_TOOLS_WORKSPACE_FILE_SYSTEM_H_
