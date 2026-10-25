#include "mcp/local_tools/workspace_catalog.h"

#include <algorithm>
#include <system_error>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"

#include "core/shell_util.h"
#include "core/status_macros.h"

namespace slop::mcp::local_tools {
namespace {

std::string RemoveOneLineEnding(std::string text) {
  if (!text.empty() && text.back() == '\n') text.pop_back();
  if (!text.empty() && text.back() == '\r') text.pop_back();
  return text;
}

absl::StatusOr<CommandResult> RunGit(const std::filesystem::path& directory, absl::string_view arguments) {
  const std::string command = absl::StrCat("git -C ", EscapeShellArg(directory.string()), " ", arguments);
  return RunCommand(command, /*cancellation=*/nullptr, /*input=*/"", /*timeout_seconds=*/10);
}

absl::StatusOr<std::filesystem::path> CanonicalDirectory(const std::filesystem::path& path,
                                                          absl::string_view description) {
  std::error_code error;
  const std::filesystem::path canonical = std::filesystem::canonical(path, error);
  if (error) {
    return absl::NotFoundError(absl::StrCat("Cannot resolve ", description, ": ", error.message()));
  }
  if (!std::filesystem::is_directory(canonical, error) || error) {
    return absl::InvalidArgumentError(absl::StrCat(description, " is not an accessible directory"));
  }
  return canonical;
}

absl::StatusOr<std::filesystem::path> ReadGitDirectory(const std::filesystem::path& directory,
                                                        absl::string_view arguments) {
  ASSIGN_OR_RETURN(auto result, RunGit(directory, arguments));
  if (result.exit_code != 0) {
    return absl::UnavailableError(absl::StrCat("Git workspace discovery failed: ", result.stderr_out));
  }
  std::string output = RemoveOneLineEnding(result.stdout_out);
  if (output.empty()) {
    return absl::DataLossError("Git returned an empty directory during workspace discovery");
  }
  return CanonicalDirectory(std::filesystem::path(output), "Git directory");
}

std::vector<std::filesystem::path> ParseWorktreePaths(absl::string_view output) {
  constexpr absl::string_view kWorktreePrefix = "worktree ";
  std::vector<std::filesystem::path> paths;
  for (const absl::string_view field : absl::StrSplit(output, '\0', absl::SkipEmpty())) {
    if (absl::StartsWith(field, kWorktreePrefix)) {
      paths.emplace_back(std::string(field.substr(kWorktreePrefix.size())));
    }
  }
  return paths;
}

}  // namespace

WorkspaceCatalog::WorkspaceCatalog(std::filesystem::path launch_directory, std::filesystem::path git_top_level,
                                   std::filesystem::path git_common_directory, bool is_git_repository)
    : launch_directory_(std::move(launch_directory)),
      git_top_level_(std::move(git_top_level)),
      git_common_directory_(std::move(git_common_directory)),
      is_git_repository_(is_git_repository) {}

absl::StatusOr<WorkspaceCatalog> WorkspaceCatalog::Create() {
  std::error_code error;
  const std::filesystem::path launch_directory = std::filesystem::current_path(error);
  if (error) {
    return absl::UnavailableError(absl::StrCat("Cannot capture launch directory: ", error.message()));
  }
  return Create(launch_directory);
}

absl::StatusOr<WorkspaceCatalog> WorkspaceCatalog::Create(const std::filesystem::path& launch_directory) {
  ASSIGN_OR_RETURN(const std::filesystem::path canonical_launch,
                   CanonicalDirectory(launch_directory, "Launch directory"));

  ASSIGN_OR_RETURN(auto top_level_result, RunGit(canonical_launch, "rev-parse --show-toplevel"));
  if (top_level_result.exit_code != 0) {
    return WorkspaceCatalog(canonical_launch, {}, {}, false);
  }
  const std::string top_level_text = RemoveOneLineEnding(top_level_result.stdout_out);
  ASSIGN_OR_RETURN(const std::filesystem::path git_top_level,
                   CanonicalDirectory(std::filesystem::path(top_level_text), "Git worktree root"));
  ASSIGN_OR_RETURN(const std::filesystem::path git_common_directory,
                   ReadGitDirectory(canonical_launch, "rev-parse --path-format=absolute --git-common-dir"));
  return WorkspaceCatalog(canonical_launch, git_top_level, git_common_directory, true);
}

absl::StatusOr<std::vector<Workspace>> WorkspaceCatalog::List() const {
  std::vector<Workspace> workspaces = {{"launch", launch_directory_, true, false}};
  if (!is_git_repository_) return workspaces;

  ASSIGN_OR_RETURN(auto result, RunGit(git_top_level_, "worktree list --porcelain -z"));
  if (result.exit_code != 0) {
    return absl::UnavailableError(absl::StrCat("Git worktree discovery failed: ", result.stderr_out));
  }

  for (const std::filesystem::path& listed_path : ParseWorktreePaths(result.stdout_out)) {
    auto root_or = CanonicalDirectory(listed_path, "Git worktree");
    if (!root_or.ok()) continue;  // Ignore prunable worktrees.
    const std::filesystem::path& root = *root_or;

    if (root == launch_directory_) {
      workspaces.front().is_git_worktree_root = true;
      continue;
    }
    if (std::any_of(workspaces.begin(), workspaces.end(),
                    [&root](const Workspace& workspace) { return workspace.root == root; })) {
      continue;
    }

    ASSIGN_OR_RETURN(const std::filesystem::path worktree_common_directory,
                     ReadGitDirectory(root, "rev-parse --path-format=absolute --git-common-dir"));
    if (worktree_common_directory != git_common_directory_) continue;

    workspaces.push_back({absl::StrCat("worktree:", root.string()), root, false, true});
  }
  return workspaces;
}

absl::StatusOr<Workspace> WorkspaceCatalog::Resolve(absl::string_view workspace_id) const {
  ASSIGN_OR_RETURN(const std::vector<Workspace> workspaces, List());
  for (const Workspace& workspace : workspaces) {
    if (workspace.id == workspace_id) return workspace;
  }
  return absl::NotFoundError(absl::StrCat("Workspace is unavailable: ", workspace_id));
}

}  // namespace slop::mcp::local_tools
