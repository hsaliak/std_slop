#include "mcp/local_tools/server.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"

#include "core/database.h"
#include "core/json_utils.h"
#include "core/shell_util.h"
#include "core/status_macros.h"
#include "mcp/local_tools/workspace_catalog.h"
#include "tools/tool_executor.h"

#include <gtest/gtest.h>

namespace slop::mcp::local_tools {
namespace {

nlohmann::json CallTool(const mcp::server::Server& server, int id, const std::string& name,
                        const nlohmann::json& arguments) {
  const nlohmann::json request = {
      {"jsonrpc", "2.0"},
      {"id", id},
      {"method", "tools/call"},
      {"params",
       {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}}},
        {"name", name},
        {"arguments", arguments}}},
  };
  const auto response = server.Dispatch(json_dump(request));
  if (!response.has_value()) return nlohmann::json::object();
  return *response;
}

std::filesystem::path MakeTempDirectory() {
  const std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / absl::StrCat("local_file_tools_", absl::ToUnixNanos(absl::Now()));
  std::error_code error;
  std::filesystem::create_directories(path, error);
  EXPECT_FALSE(error) << error.message();
  return path;
}

void WriteFile(const std::filesystem::path& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open()) << path.string();
  output << content;
  ASSERT_TRUE(output.good()) << path.string();
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  EXPECT_TRUE(input.is_open()) << path.string();
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

absl::Status RunGit(const std::filesystem::path& directory, absl::string_view arguments) {
  const std::string command = absl::StrCat("git -C ", EscapeShellArg(directory.string()), " ", arguments);
  auto result = RunCommand(command);
  if (!result.ok()) return result.status();
  if (result->exit_code != 0) return absl::InternalError(result->stderr_out);
  return absl::OkStatus();
}

absl::Status CreateGitRepository(const std::filesystem::path& root) {
  std::error_code error;
  std::filesystem::create_directories(root, error);
  if (error) return absl::UnavailableError(error.message());
  RETURN_IF_ERROR(RunGit(root, "init --quiet -b main"));
  RETURN_IF_ERROR(RunGit(root, "config user.name local-tool-test"));
  RETURN_IF_ERROR(RunGit(root, "config user.email local-tool-test@example.invalid"));
  WriteFile(root / "data.txt", "main content\n");
  RETURN_IF_ERROR(RunGit(root, "add data.txt"));
  return RunGit(root, "commit --quiet -m initial");
}

TEST(LocalFileToolsTest, ReadListGrepWriteAndEditUseSharedWorkspaceFilesystem) {
  const std::filesystem::path root = MakeTempDirectory();
  ASSERT_FALSE(root.empty());
  absl::Cleanup remove_root = [&root] {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  };
  std::filesystem::create_directories(root / "src");
  WriteFile(root / "src" / "data.txt", "one\nneedle two\nthree\n");

  auto catalog = WorkspaceCatalog::Create(root);
  ASSERT_TRUE(catalog.ok()) << catalog.status().message();
  auto server = CreateServer(std::move(*catalog));
  ASSERT_TRUE(server.ok()) << server.status().message();

  const auto read = CallTool(*server, 1, "read_file",
                             {{"path", "src/data.txt"}, {"start_line", 2}, {"line_numbers", true}});
  ASSERT_TRUE(read.contains("result"));
  EXPECT_FALSE(read["result"]["isError"].get<bool>());
  EXPECT_EQ(read["result"]["content"][0]["text"], "2: needle two\n3: three\n");

  const auto listing = CallTool(*server, 2, "list_directory", {{"path", "src"}});
  ASSERT_TRUE(listing.contains("result"));
  EXPECT_TRUE(absl::StrContains(listing["result"]["structuredContent"]["entries"].get<std::string>(),
                                "File: data.txt"));

  const auto grep = CallTool(*server, 3, "grep", {{"path", "src"}, {"pattern", "needle"}, {"fixed_strings", true}});
  ASSERT_TRUE(grep.contains("result"));
  EXPECT_TRUE(absl::StrContains(grep["result"]["structuredContent"]["matches"].get<std::string>(),
                                "src/data.txt:2:needle two"));

  const auto written = CallTool(*server, 4, "write_file", {{"path", "src/new.txt"}, {"content", "alpha beta\n"}});
  ASSERT_TRUE(written.contains("result"));
  EXPECT_FALSE(written["result"]["isError"].get<bool>());

  const nlohmann::json edits = {{{"op", "replace"}, {"find", "beta"}, {"text", "gamma"}}};
  const auto edited = CallTool(*server, 5, "edit_tool", {{"path", "src/new.txt"}, {"edits", edits}});
  ASSERT_TRUE(edited.contains("result"));
  EXPECT_FALSE(edited["result"]["isError"].get<bool>());
  EXPECT_EQ(ReadFile(root / "src" / "new.txt"), "alpha gamma\n");
}

TEST(LocalFileToolsTest, WorkspaceSelectionTargetsOnlyTheChosenWorktree) {
  const std::filesystem::path temporary_root = MakeTempDirectory();
  ASSERT_FALSE(temporary_root.empty());
  absl::Cleanup remove_root = [&temporary_root] {
    std::error_code error;
    std::filesystem::remove_all(temporary_root, error);
  };
  const std::filesystem::path repository = temporary_root / "repository with spaces";
  const std::filesystem::path worktree = temporary_root / "feature worktree";
  ASSERT_TRUE(CreateGitRepository(repository).ok());
  ASSERT_TRUE(RunGit(repository, absl::StrCat("worktree add --quiet -b feature ",
                                              EscapeShellArg(worktree.string())))
                  .ok());
  auto catalog = WorkspaceCatalog::Create(repository);
  ASSERT_TRUE(catalog.ok()) << catalog.status().message();
  auto server = CreateServer(std::move(*catalog));
  ASSERT_TRUE(server.ok()) << server.status().message();
  const std::string worktree_id = absl::StrCat("worktree:", std::filesystem::canonical(worktree).string());

  const auto write = CallTool(*server, 1, "write_file",
                              {{"workspace", worktree_id}, {"path", "data.txt"}, {"content", "feature content\n"}});
  ASSERT_TRUE(write.contains("result"));
  EXPECT_FALSE(write["result"]["isError"].get<bool>());

  const auto main_read = CallTool(*server, 2, "read_file", {{"path", "data.txt"}});
  ASSERT_TRUE(main_read.contains("result"));
  EXPECT_EQ(main_read["result"]["content"][0]["text"], "main content\n");
  const auto feature_read =
      CallTool(*server, 3, "read_file", {{"workspace", worktree_id}, {"path", "data.txt"}});
  ASSERT_TRUE(feature_read.contains("result"));
  EXPECT_EQ(feature_read["result"]["content"][0]["text"], "feature content\n");

  const auto feature_cwd =
      CallTool(*server, 4, "execute_bash", {{"workspace", worktree_id}, {"command", "pwd -P"}});
  ASSERT_TRUE(feature_cwd.contains("result"));
  EXPECT_EQ(feature_cwd["result"]["structuredContent"]["stdout"],
            std::filesystem::canonical(worktree).string() + "\n");
}

TEST(LocalFileToolsTest, ShellUsesChildCwdBoundsTimeoutAndHandlesExitCodes) {
  const std::filesystem::path root = MakeTempDirectory();
  ASSERT_FALSE(root.empty());
  absl::Cleanup remove_root = [&root] {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  };
  std::filesystem::create_directories(root / "nested");
  auto catalog = WorkspaceCatalog::Create(root);
  ASSERT_TRUE(catalog.ok()) << catalog.status().message();
  auto server = CreateServer(std::move(*catalog));
  ASSERT_TRUE(server.ok()) << server.status().message();
  const std::filesystem::path original_cwd = std::filesystem::current_path();

  const auto cwd = CallTool(*server, 1, "execute_bash", {{"command", "pwd -P"}, {"cwd", "nested"}});
  ASSERT_TRUE(cwd.contains("result"));
  EXPECT_FALSE(cwd["result"]["isError"].get<bool>());
  EXPECT_EQ(cwd["result"]["structuredContent"]["stdout"],
            std::filesystem::canonical(root / "nested").string() + "\n");
  EXPECT_EQ(std::filesystem::current_path(), original_cwd);

  const auto nonzero = CallTool(*server, 2, "execute_bash", {{"command", "exit 7"}});
  ASSERT_TRUE(nonzero.contains("result"));
  EXPECT_TRUE(nonzero["result"]["isError"].get<bool>());
  EXPECT_EQ(nonzero["result"]["structuredContent"]["exit_code"], 7);
  const auto allowed_nonzero =
      CallTool(*server, 3, "execute_bash", {{"command", "exit 7"}, {"allow_nonzero_exit", true}});
  ASSERT_TRUE(allowed_nonzero.contains("result"));
  EXPECT_FALSE(allowed_nonzero["result"]["isError"].get<bool>());

  const auto bad_cwd = CallTool(*server, 4, "execute_bash",
                                {{"command", "printf should-not-run"}, {"cwd", "../outside"}});
  ASSERT_TRUE(bad_cwd.contains("result"));
  EXPECT_TRUE(bad_cwd["result"]["isError"].get<bool>());

  const auto timeout =
      CallTool(*server, 5, "execute_bash", {{"command", "sleep 5"}, {"timeout_seconds", 1}});
  ASSERT_TRUE(timeout.contains("result"));
  EXPECT_TRUE(timeout["result"]["isError"].get<bool>());

  const auto output_limit = CallTool(*server, 6, "execute_bash",
                                     {{"command", "while :; do printf x; done"}, {"timeout_seconds", 5}});
  ASSERT_TRUE(output_limit.contains("result"));
  EXPECT_TRUE(output_limit["result"]["isError"].get<bool>());
  EXPECT_EQ(output_limit["result"]["structuredContent"]["error"]["message"],
            "Command output exceeded the configured byte limit");

  const auto too_long =
      CallTool(*server, 7, "execute_bash", {{"command", "true"}, {"timeout_seconds", 46}});
  ASSERT_TRUE(too_long.contains("result"));
  EXPECT_TRUE(too_long["result"]["isError"].get<bool>());
}

TEST(LocalFileToolsTest, InvalidPathsAndSymlinksCannotModifyOutsideFiles) {
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
  WriteFile(outside / "safe.txt", "safe\n");
  std::filesystem::create_directory_symlink(outside, root / "escape", error);
  ASSERT_FALSE(error) << error.message();

  auto catalog = WorkspaceCatalog::Create(root);
  ASSERT_TRUE(catalog.ok()) << catalog.status().message();
  auto server = CreateServer(std::move(*catalog));
  ASSERT_TRUE(server.ok()) << server.status().message();

  const auto traversal = CallTool(*server, 1, "write_file", {{"path", "../outside.txt"}, {"content", "bad"}});
  ASSERT_TRUE(traversal.contains("result"));
  EXPECT_TRUE(traversal["result"]["isError"].get<bool>());
  EXPECT_FALSE(std::filesystem::exists(root.parent_path() / "outside.txt"));

  const auto symlink = CallTool(*server, 2, "write_file",
                                {{"path", "escape/safe.txt"}, {"content", "changed"}});
  ASSERT_TRUE(symlink.contains("result"));
  EXPECT_TRUE(symlink["result"]["isError"].get<bool>());
  EXPECT_EQ(ReadFile(outside / "safe.txt"), "safe\n");
}

TEST(LocalFileToolsTest, ToolSchemasRejectMalformedArgumentsBeforeHandler) {
  const std::filesystem::path root = MakeTempDirectory();
  ASSERT_FALSE(root.empty());
  absl::Cleanup remove_root = [&root] {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  };
  auto catalog = WorkspaceCatalog::Create(root);
  ASSERT_TRUE(catalog.ok()) << catalog.status().message();
  auto server = CreateServer(std::move(*catalog));
  ASSERT_TRUE(server.ok()) << server.status().message();

  const auto response = CallTool(*server, 1, "write_file", {{"path", "file.txt"}, {"content", 42}});
  ASSERT_TRUE(response.contains("result"));
  EXPECT_TRUE(response["result"]["isError"].get<bool>());
  EXPECT_FALSE(std::filesystem::exists(root / "file.txt"));

  const auto invalid_line = CallTool(*server, 2, "read_file",
                                     {{"path", "missing.txt"}, {"start_line", 2147483648LL}});
  ASSERT_TRUE(invalid_line.contains("result"));
  EXPECT_TRUE(invalid_line["result"]["isError"].get<bool>());
}

TEST(LocalFileToolsTest, NativeAndMcpAdaptersShareWorkspaceFileOperations) {
  const std::filesystem::path root = std::filesystem::current_path() /
                                     absl::StrCat("shared_file_ops_", absl::ToUnixNanos(absl::Now()));
  std::error_code error;
  std::filesystem::create_directories(root / "src", error);
  ASSERT_FALSE(error) << error.message();
  absl::Cleanup remove_root = [&root] {
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
  };
  WriteFile(root / "src" / "data.txt", "one\nneedle two\n");

  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor.ok());
  auto catalog = WorkspaceCatalog::Create(root);
  ASSERT_TRUE(catalog.ok()) << catalog.status().message();
  auto server = CreateServer(std::move(*catalog));
  ASSERT_TRUE(server.ok()) << server.status().message();

  const std::string native_path = (root.filename() / "src" / "data.txt").string();
  auto native_read = (*executor)->Execute("read_file", {{"path", native_path}});
  ASSERT_TRUE(native_read.ok()) << native_read.status().message();
  const auto mcp_read = CallTool(*server, 1, "read_file", {{"path", "src/data.txt"}});
  ASSERT_TRUE(mcp_read.contains("result"));
  EXPECT_EQ(*native_read, mcp_read["result"]["content"][0]["text"].get<std::string>());

  auto native_grep = (*executor)->Execute("grep", {{"path", (root.filename() / "src").string()},
                                                       {"pattern", "needle"},
                                                       {"fixed_strings", true}});
  ASSERT_TRUE(native_grep.ok()) << native_grep.status().message();
  const auto mcp_grep = CallTool(*server, 2, "grep", {{"path", "src"}, {"pattern", "needle"},
                                                        {"fixed_strings", true}});
  ASSERT_TRUE(mcp_grep.contains("result"));
  EXPECT_TRUE(absl::StrContains(*native_grep, "data.txt:2:needle two"));
  EXPECT_TRUE(absl::StrContains(mcp_grep["result"]["structuredContent"]["matches"].get<std::string>(),
                                "data.txt:2:needle two"));

  const std::string native_write_path = (root.filename() / "src" / "native.txt").string();
  auto native_write = (*executor)->Execute("write_file", {{"path", native_write_path}, {"content", "alpha beta"}});
  ASSERT_TRUE(native_write.ok()) << native_write.status().message();
  const auto mcp_write = CallTool(*server, 3, "write_file", {{"path", "src/mcp.txt"}, {"content", "alpha beta"}});
  ASSERT_TRUE(mcp_write.contains("result"));
  EXPECT_EQ(ReadFile(root / "src" / "native.txt"), ReadFile(root / "src" / "mcp.txt"));

  const nlohmann::json edits = {{{"op", "replace"}, {"find", "beta"}, {"text", "gamma"}}};
  auto native_edit = (*executor)->Execute("edit_tool", {{"path", native_write_path}, {"edits", edits}});
  ASSERT_TRUE(native_edit.ok()) << native_edit.status().message();
  const auto mcp_edit = CallTool(*server, 4, "edit_tool", {{"path", "src/mcp.txt"}, {"edits", edits}});
  ASSERT_TRUE(mcp_edit.contains("result"));
  EXPECT_FALSE(mcp_edit["result"]["isError"].get<bool>());
  EXPECT_EQ(ReadFile(root / "src" / "native.txt"), "alpha gamma");
  EXPECT_EQ(ReadFile(root / "src" / "mcp.txt"), ReadFile(root / "src" / "native.txt"));
}

}  // namespace
}  // namespace slop::mcp::local_tools
