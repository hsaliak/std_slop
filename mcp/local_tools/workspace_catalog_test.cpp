#include "mcp/local_tools/workspace_catalog.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>

#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"

#include "core/shell_util.h"
#include "core/status_macros.h"

#include <gtest/gtest.h>

namespace slop::mcp::local_tools {
namespace {

absl::Status RunGit(const std::filesystem::path& directory, absl::string_view arguments) {
  const std::string command = absl::StrCat("git -C ", EscapeShellArg(directory.string()), " ", arguments);
  auto result = RunCommand(command);
  if (!result.ok()) return result.status();
  if (result->exit_code != 0) {
    return absl::InternalError(absl::StrCat("git command failed: ", result->stderr_out));
  }
  return absl::OkStatus();
}

std::filesystem::path MakeTempDirectory() {
  const std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / absl::StrCat("mcp_workspace_", absl::ToUnixNanos(absl::Now()));
  std::error_code error;
  std::filesystem::create_directories(path, error);
  EXPECT_FALSE(error) << error.message();
  return path;
}

absl::Status CreateGitRepository(const std::filesystem::path& root) {
  std::error_code error;
  std::filesystem::create_directories(root, error);
  if (error) return absl::UnavailableError(error.message());
  RETURN_IF_ERROR(RunGit(root, "init --quiet -b main"));
  RETURN_IF_ERROR(RunGit(root, "config user.name workspace-test"));
  RETURN_IF_ERROR(RunGit(root, "config user.email workspace-test@example.invalid"));
  return RunGit(root, "commit --allow-empty --quiet -m initial");
}

TEST(WorkspaceCatalogTest, NonGitLaunchIsTheOnlyDefaultWorkspace) {
  const std::filesystem::path root = MakeTempDirectory();
  ASSERT_FALSE(root.empty());
  absl::Cleanup remove_root = [&root] {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  };

  auto catalog = WorkspaceCatalog::Create(root);
  ASSERT_TRUE(catalog.ok()) << catalog.status().message();
  auto workspaces = catalog->List();
  ASSERT_TRUE(workspaces.ok()) << workspaces.status().message();
  ASSERT_EQ(workspaces->size(), 1);
  EXPECT_EQ(workspaces->front().id, "launch");
  EXPECT_EQ(workspaces->front().root, std::filesystem::canonical(root));
  EXPECT_TRUE(workspaces->front().is_default);

  auto unknown = catalog->Resolve("worktree:/elsewhere");
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.status().code(), absl::StatusCode::kNotFound);
}

TEST(WorkspaceCatalogTest, DiscoversWorktreeAddedAfterCatalogCreation) {
  const std::filesystem::path temp_root = MakeTempDirectory();
  ASSERT_FALSE(temp_root.empty());
  absl::Cleanup remove_root = [&temp_root] {
    std::error_code error;
    std::filesystem::remove_all(temp_root, error);
  };
  const std::filesystem::path repository = temp_root / "repository with spaces";
  const std::filesystem::path worktree = temp_root / "feature worktree";
  ASSERT_TRUE(CreateGitRepository(repository).ok());

  auto catalog = WorkspaceCatalog::Create(repository);
  ASSERT_TRUE(catalog.ok()) << catalog.status().message();
  auto initial = catalog->List();
  ASSERT_TRUE(initial.ok()) << initial.status().message();
  ASSERT_EQ(initial->size(), 1);

  ASSERT_TRUE(RunGit(repository, absl::StrCat("worktree add --quiet -b feature ",
                                             EscapeShellArg(worktree.string())))
                  .ok());
  std::error_code error;
  EXPECT_TRUE(std::filesystem::is_regular_file(worktree / ".git", error));
  ASSERT_FALSE(error);

  auto updated = catalog->List();
  ASSERT_TRUE(updated.ok()) << updated.status().message();
  ASSERT_EQ(updated->size(), 2);
  const auto feature = std::find_if(updated->begin(), updated->end(), [](const Workspace& workspace) {
    return workspace.id != "launch";
  });
  ASSERT_NE(feature, updated->end());
  EXPECT_EQ(feature->root, std::filesystem::canonical(worktree));
  EXPECT_EQ(feature->id, absl::StrCat("worktree:", feature->root.string()));
  EXPECT_FALSE(feature->is_default);

  auto resolved = catalog->Resolve(feature->id);
  ASSERT_TRUE(resolved.ok()) << resolved.status().message();
  EXPECT_EQ(resolved->root, feature->root);

  ASSERT_TRUE(RunGit(repository, absl::StrCat("worktree remove ", EscapeShellArg(worktree.string()))).ok());
  auto removed = catalog->Resolve(feature->id);
  ASSERT_FALSE(removed.ok());
  EXPECT_EQ(removed.status().code(), absl::StatusCode::kNotFound);
}

TEST(WorkspaceCatalogTest, LaunchSubdirectoryAndWorktreeRootAreSeparateEntries) {
  const std::filesystem::path temp_root = MakeTempDirectory();
  ASSERT_FALSE(temp_root.empty());
  absl::Cleanup remove_root = [&temp_root] {
    std::error_code error;
    std::filesystem::remove_all(temp_root, error);
  };
  const std::filesystem::path repository = temp_root / "repository";
  ASSERT_TRUE(CreateGitRepository(repository).ok());
  const std::filesystem::path source_directory = repository / "src";
  std::error_code error;
  std::filesystem::create_directory(source_directory, error);
  ASSERT_FALSE(error) << error.message();

  auto catalog = WorkspaceCatalog::Create(source_directory);
  ASSERT_TRUE(catalog.ok()) << catalog.status().message();
  auto workspaces = catalog->List();
  ASSERT_TRUE(workspaces.ok()) << workspaces.status().message();
  ASSERT_EQ(workspaces->size(), 2);

  EXPECT_EQ(workspaces->front().id, "launch");
  EXPECT_EQ(workspaces->front().root, std::filesystem::canonical(source_directory));
  EXPECT_EQ(workspaces->back().id, absl::StrCat("worktree:", std::filesystem::canonical(repository).string()));
}

}  // namespace
}  // namespace slop::mcp::local_tools
