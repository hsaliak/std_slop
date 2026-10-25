#include "mcp/client/runtime.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "nlohmann/json.hpp"

#include "core/http_client.h"
#include "core/shell_util.h"
#include "core/json_utils.h"
#include "mcp/client/registry.h"
#include "mcp/client/token_store.h"
#include "mcp/protocol.h"
#include "tools/tool_executor.h"

#include <gtest/gtest.h>

namespace slop::mcp {
namespace {

class FakeHttpClient : public HttpClient {
 public:
  absl::StatusOr<HttpResponse> PostStreamWithResponse(const std::string& url, const std::string& body,
                                                      const std::vector<std::string>& headers,
                                                      ChunkCallback on_chunk) override {
    last_url = url;
    last_headers = headers;
    request_count++;
    auto request = json_parse(body);
    if (!request || !request->is_object()) return absl::InvalidArgumentError("invalid request");
    const std::string method = json_get_or(*request, "method", std::string{});
    if (method == "notifications/initialized") return HttpResponse{202, "", {}};
    nlohmann::json response = {{"jsonrpc", "2.0"}};
    const auto* id = json_at(*request, "id");
    if (id != nullptr) response["id"] = *id;
    if (method == "initialize") {
      response["result"] = {{"protocolVersion", std::string(kClassicProtocolVersion)},
                            {"capabilities", nlohmann::json::object()}};
    } else if (method == "server/discover") {
      response["result"] = {{"supportedVersions", {std::string(kModernProtocolVersion)}},
                            {"capabilities", {{"tools", nlohmann::json::object()}}}};
    } else if (method == "tools/list") {
      response["result"] = {
          {"tools", nlohmann::json::array(
                        {{{"name", "search"}, {"description", "Search"}, {"inputSchema", {{"type", "object"}}}}})}};
    } else if (method == "tools/call") {
      response["result"] = {{"content", nlohmann::json::array({{{"type", "text"}, {"text", "ok"}}})}};
    } else {
      response["result"] = nlohmann::json::object();
    }
    const std::string response_body = json_dump(response);
    if (on_chunk) {
      const absl::Status chunk_status = on_chunk(response_body);
      if (!chunk_status.ok()) return chunk_status;
    }
    return HttpResponse{200, response_body, {{"content-type", "application/json"}}};
  }

  absl::StatusOr<HttpResponse> PostOnceStreamWithResponse(const std::string& url, const std::string& body,
                                                          const std::vector<std::string>& headers, absl::Duration,
                                                          size_t, ChunkCallback on_chunk) override {
    return PostStreamWithResponse(url, body, headers, std::move(on_chunk));
  }

  std::string last_url;
  std::vector<std::string> last_headers;
  int request_count = 0;
};

class FakeRuntimeSession : public RuntimeSession {
 public:
  FakeRuntimeSession(std::vector<Tool> tools, ToolCallResult result, absl::Status call_status = absl::OkStatus())
      : tools_(std::move(tools)), result_(std::move(result)), call_status_(std::move(call_status)) {}

  absl::StatusOr<std::vector<Tool>> ListTools() override {
    if (!list_status_.ok()) return list_status_;
    return tools_;
  }

  void SetTools(std::vector<Tool> tools) { tools_ = std::move(tools); }
  void SetListStatus(absl::Status status) { list_status_ = std::move(status); }

  absl::StatusOr<ToolCallResult> CallTool(const std::string& name, const nlohmann::json& arguments) override {
    called_tool_name = name;
    called_arguments = arguments;
    if (!call_status_.ok()) return call_status_;
    return result_;
  }

  std::string called_tool_name;
  nlohmann::json called_arguments;

 private:
  std::vector<Tool> tools_;
  ToolCallResult result_;
  absl::Status call_status_;
  absl::Status list_status_ = absl::OkStatus();
};

Tool MakeTool(const std::string& name) {
  Tool tool;
  tool.name = name;
  tool.description = absl::StrCat("description for ", name);
  tool.input_schema = nlohmann::json{{"type", "object"}};
  return tool;
}

ServerRegistryEntry MakeEntry(const std::string& name) {
  ServerRegistryEntry entry;
  entry.name = name;
  entry.url = absl::StrCat("https://", name, ".example/mcp");
  entry.auth = kAuthNone;
  entry.enabled = true;
  return entry;
}

ServerRegistryEntry MakeStdioEntry(const std::string& name, const std::string& command,
                                   std::vector<std::string> args = {}, bool enabled = true) {
  ServerRegistryEntry entry;
  entry.name = name;
  entry.transport = kTransportStdio;
  entry.command = command;
  entry.args = std::move(args);
  entry.enabled = enabled;
  return entry;
}

std::string LocalToolsServerPath() {
  const char* test_srcdir = std::getenv("TEST_SRCDIR");
  const char* test_workspace = std::getenv("TEST_WORKSPACE");
  if (test_srcdir == nullptr || test_workspace == nullptr) return "";
  return absl::StrCat(test_srcdir, "/", test_workspace, "/mcp/local_tools/local_tools_server");
}

absl::Status RunGit(const std::filesystem::path& directory, const std::string& arguments) {
  auto result = RunCommand(absl::StrCat("git -C ", EscapeShellArg(directory.string()), " ", arguments));
  if (!result.ok()) return result.status();
  if (result->exit_code != 0) return absl::InternalError(result->stderr_out);
  return absl::OkStatus();
}

std::string EchoServerPath() {
  const char* test_srcdir = std::getenv("TEST_SRCDIR");
  const char* test_workspace = std::getenv("TEST_WORKSPACE");
  if (test_srcdir == nullptr || test_workspace == nullptr) return "";
  return absl::StrCat(test_srcdir, "/", test_workspace, "/mcp/server/echo_server");
}

std::string TempRegistryPath() {
  return absl::StrCat(::testing::TempDir(), "/std_slop_mcp_runtime_", absl::ToUnixNanos(absl::Now()), ".ini");
}

bool HasHeader(const std::vector<std::string>& headers, const std::string& expected) {
  return std::find(headers.begin(), headers.end(), expected) != headers.end();
}

TEST(McpRuntimeTest, DuplicateServerToolNamesDoNotCollide) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor.ok());
  FakeHttpClient http_client;

  const std::string registry_path = TempRegistryPath();
  ASSERT_TRUE(SaveServerRegistry(registry_path, {MakeEntry("github"), MakeEntry("gitlab")}).ok());

  std::vector<FakeRuntimeSession*> sessions;
  RuntimeOptions options;
  options.registry_path = registry_path;
  RuntimeManager manager(&db, executor->get(), &http_client, options,
                         [&sessions](const ServerRegistryEntry&, HttpClient*, const RuntimeOptions&) {
                           ToolCallResult result;
                           result.content.push_back(nlohmann::json{{"type", "text"}, {"text", "ok"}});
                           auto session =
                               std::make_unique<FakeRuntimeSession>(std::vector<Tool>{MakeTool("search")}, result);
                           sessions.push_back(session.get());
                           return absl::StatusOr<std::unique_ptr<RuntimeSession>>(std::move(session));
                         });
  ASSERT_TRUE(manager.Start().ok());

  auto tools = db.GetTopLevelTools();
  ASSERT_TRUE(tools.ok());
  const auto has_tool = [&](const std::string& name) {
    return std::any_of(tools->begin(), tools->end(), [&](const Database::Tool& tool) { return tool.name == name; });
  };
  EXPECT_TRUE(has_tool("mcp_github_search"));
  EXPECT_TRUE(has_tool("mcp_gitlab_search"));

  auto output = (*executor)->Execute("mcp_github_search", nlohmann::json{{"query", "repo"}});
  ASSERT_TRUE(output.ok()) << output.status();
  EXPECT_EQ(sessions[0]->called_tool_name, "search");
  EXPECT_EQ(sessions[0]->called_arguments["query"], "repo");
}

TEST(McpRuntimeTest, MailModeGateBlocksImportedToolsBeforeRemoteCall) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor.ok());
  FakeHttpClient http_client;

  const std::string registry_path = TempRegistryPath();
  ASSERT_TRUE(SaveServerRegistry(registry_path, {MakeEntry("remote")}).ok());
  FakeRuntimeSession* runtime_session = nullptr;
  RuntimeOptions options;
  options.registry_path = registry_path;
  RuntimeManager manager(&db, executor->get(), &http_client, options,
                         [&runtime_session](const ServerRegistryEntry&, HttpClient*, const RuntimeOptions&) {
                           ToolCallResult result;
                           result.content.push_back(nlohmann::json{{"type", "text"}, {"text", "ok"}});
                           auto session = std::make_unique<FakeRuntimeSession>(std::vector<Tool>{MakeTool("search")},
                                                                               result);
                           runtime_session = session.get();
                           return absl::StatusOr<std::unique_ptr<RuntimeSession>>(std::move(session));
                         });
  ASSERT_TRUE(manager.Start().ok());
  ASSERT_NE(runtime_session, nullptr);
  (*executor)->SetMailMode(true);

  const char* old_branch_value = std::getenv("SLOP_FORCE_BRANCH_NAME");
  const std::optional<std::string> old_branch =
      old_branch_value == nullptr ? std::nullopt : std::optional<std::string>(old_branch_value);
  const char* old_skip_value = std::getenv("SLOP_SKIP_STAGING_CHECK");
  const std::optional<std::string> old_skip =
      old_skip_value == nullptr ? std::nullopt : std::optional<std::string>(old_skip_value);
  absl::Cleanup restore_environment = [&] {
    if (old_branch) {
      setenv("SLOP_FORCE_BRANCH_NAME", old_branch->c_str(), 1);
    } else {
      unsetenv("SLOP_FORCE_BRANCH_NAME");
    }
    if (old_skip) {
      setenv("SLOP_SKIP_STAGING_CHECK", old_skip->c_str(), 1);
    } else {
      unsetenv("SLOP_SKIP_STAGING_CHECK");
    }
  };

  ASSERT_EQ(setenv("SLOP_FORCE_BRANCH_NAME", "main", 1), 0);
  ASSERT_EQ(unsetenv("SLOP_SKIP_STAGING_CHECK"), 0);
  const auto blocked = (*executor)->Execute("mcp_remote_search", {{"query", "secret"}});
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.status().code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_TRUE(runtime_session->called_tool_name.empty());

  ASSERT_EQ(setenv("SLOP_FORCE_BRANCH_NAME", "slop/staging/test", 1), 0);
  const auto allowed = (*executor)->Execute("mcp_remote_search", {{"query", "safe"}});
  ASSERT_TRUE(allowed.ok()) << allowed.status().message();
  EXPECT_EQ(runtime_session->called_tool_name, "search");
  EXPECT_EQ(runtime_session->called_arguments["query"], "safe");
}

TEST(McpRuntimeTest, StartsStdioServerWithoutHttpClientAndRoutesCalls) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor.ok());
  const std::string registry_path = TempRegistryPath();
  const std::string echo_server = EchoServerPath();
  ASSERT_FALSE(echo_server.empty());
  ASSERT_TRUE(SaveServerRegistry(registry_path, {MakeStdioEntry("local", echo_server)}).ok());

  RuntimeOptions options;
  options.registry_path = registry_path;
  auto manager = StartMcpRuntime(&db, executor->get(), nullptr, options);

  ASSERT_TRUE(manager.ok()) << manager.status();
  EXPECT_EQ((*manager)->active_server_count(), 1);
  auto tools = db.GetTopLevelTools();
  ASSERT_TRUE(tools.ok());
  EXPECT_NE(std::find_if(tools->begin(), tools->end(),
                         [](const Database::Tool& tool) { return tool.name == "mcp_local_echo"; }),
            tools->end());

  auto output = (*executor)->Execute("mcp_local_echo", nlohmann::json{{"text", "stdio runtime"}});
  ASSERT_TRUE(output.ok()) << output.status();
  auto parsed = json_parse(*output);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(json_get_or(*parsed, "structured_content", nlohmann::json::object()),
            nlohmann::json({{"text", "stdio runtime"}}));
  ASSERT_TRUE((*manager)->RefreshCatalogs().ok());
  manager->reset();
}

TEST(McpRuntimeTest, LocalToolsRuntimeUsesWorktreesAndFileToolsPerCall) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor.ok());
  const std::string local_server = LocalToolsServerPath();
  ASSERT_FALSE(local_server.empty());

  const std::filesystem::path temporary_root =
      std::filesystem::path(::testing::TempDir()) / absl::StrCat("mcp_runtime_local_", absl::ToUnixNanos(absl::Now()));
  std::error_code filesystem_error;
  std::filesystem::create_directories(temporary_root, filesystem_error);
  ASSERT_FALSE(filesystem_error) << filesystem_error.message();
  absl::Cleanup remove_temporary_root = [&temporary_root] {
    std::error_code error;
    std::filesystem::remove_all(temporary_root, error);
  };

  const std::filesystem::path repository = temporary_root / "repository";
  const std::filesystem::path worktree = temporary_root / "feature worktree";
  std::filesystem::create_directories(repository, filesystem_error);
  ASSERT_FALSE(filesystem_error) << filesystem_error.message();
  ASSERT_TRUE(RunGit(repository, "init --quiet -b main").ok());
  ASSERT_TRUE(RunGit(repository, "config user.name runtime-test").ok());
  ASSERT_TRUE(RunGit(repository, "config user.email runtime-test@example.invalid").ok());
  {
    std::ofstream file(repository / "data.txt", std::ios::binary);
    ASSERT_TRUE(file.is_open());
    file << "main content\n";
    ASSERT_TRUE(file.good());
  }
  ASSERT_TRUE(RunGit(repository, "add data.txt").ok());
  ASSERT_TRUE(RunGit(repository, "commit --quiet -m initial").ok());

  const std::string registry_path = TempRegistryPath();
  absl::Cleanup remove_registry = [&registry_path] {
    std::error_code error;
    std::filesystem::remove(registry_path, error);
  };
  const std::string launch_command =
      absl::StrCat("cd ", EscapeShellArg(repository.string()), " && exec ", EscapeShellArg(local_server));
  ServerRegistryEntry entry = MakeStdioEntry("local", "/bin/sh", {"-c", launch_command});
  ASSERT_TRUE(SaveServerRegistry(registry_path, {entry}).ok());
  RuntimeOptions options;
  options.registry_path = registry_path;
  auto manager = StartMcpRuntime(&db, executor->get(), nullptr, options);
  ASSERT_TRUE(manager.ok()) << manager.status();
  ASSERT_EQ((*manager)->active_server_count(), 1);

  auto launch_info = (*executor)->Execute("mcp_local_workspace_info", nlohmann::json::object());
  ASSERT_TRUE(launch_info.ok()) << launch_info.status().message();
  auto parsed_info = json_parse(*launch_info);
  ASSERT_TRUE(parsed_info.has_value());
  EXPECT_EQ(json_get_or(*parsed_info, "structured_content", nlohmann::json::object())["root"],
            std::filesystem::canonical(repository).string());

  ASSERT_TRUE(RunGit(repository,
                     absl::StrCat("worktree add --quiet -b feature ", EscapeShellArg(worktree.string())))
                  .ok());
  const std::string worktree_id = absl::StrCat("worktree:", std::filesystem::canonical(worktree).string());
  auto listed = (*executor)->Execute("mcp_local_workspace_list", nlohmann::json::object());
  ASSERT_TRUE(listed.ok()) << listed.status().message();
  auto parsed_list = json_parse(*listed);
  ASSERT_TRUE(parsed_list.has_value());
  const nlohmann::json list_content = json_get_or(*parsed_list, "structured_content", nlohmann::json::object());
  bool found_worktree = false;
  for (const auto& item : list_content["workspaces"]) {
    if (json_get_or(item, "id", std::string()) == worktree_id) found_worktree = true;
  }
  EXPECT_TRUE(found_worktree);

  const nlohmann::json write_args = {
      {"workspace", worktree_id}, {"path", "data.txt"}, {"content", "feature content\n"}};
  auto write = (*executor)->Execute("mcp_local_write_file", write_args);
  ASSERT_TRUE(write.ok()) << write.status().message();
  const nlohmann::json edits = {{{"op", "replace"}, {"find", "feature"}, {"text", "edited"}}};
  auto edit = (*executor)->Execute("mcp_local_edit_tool", {{"workspace", worktree_id}, {"path", "data.txt"},
                                                              {"edits", edits}});
  ASSERT_TRUE(edit.ok()) << edit.status().message();

  auto main_read = (*executor)->Execute("mcp_local_read_file", {{"path", "data.txt"}});
  ASSERT_TRUE(main_read.ok()) << main_read.status().message();
  auto parsed_main = json_parse(*main_read);
  ASSERT_TRUE(parsed_main.has_value());
  EXPECT_EQ(json_get_or(*parsed_main, "structured_content", nlohmann::json::object())["content"], "main content\n");

  auto worktree_read = (*executor)->Execute("mcp_local_read_file", {{"workspace", worktree_id}, {"path", "data.txt"}});
  ASSERT_TRUE(worktree_read.ok()) << worktree_read.status().message();
  auto parsed_worktree = json_parse(*worktree_read);
  ASSERT_TRUE(parsed_worktree.has_value());
  EXPECT_EQ(json_get_or(*parsed_worktree, "structured_content", nlohmann::json::object())["content"], "edited content\n");

  auto shell = (*executor)->Execute("mcp_local_execute_bash", {{"workspace", worktree_id}, {"command", "pwd -P"}});
  ASSERT_TRUE(shell.ok()) << shell.status().message();
  auto parsed_shell = json_parse(*shell);
  ASSERT_TRUE(parsed_shell.has_value());
  EXPECT_EQ(json_get_or(*parsed_shell, "structured_content", nlohmann::json::object())["stdout"],
            std::filesystem::canonical(worktree).string() + "\n");
  manager->reset();
}

TEST(McpRuntimeTest, DisabledStdioServerIsNeverStarted) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor.ok());
  const std::string marker =
      absl::StrCat(::testing::TempDir(), "/disabled_mcp_server_", absl::ToUnixNanos(absl::Now()));
  std::filesystem::remove(marker);
  const std::string registry_path = TempRegistryPath();
  ASSERT_TRUE(SaveServerRegistry(registry_path, {MakeStdioEntry("disabled", "/usr/bin/touch", {marker}, false)}).ok());

  RuntimeOptions options;
  options.registry_path = registry_path;
  auto manager = StartMcpRuntime(&db, executor->get(), nullptr, options);

  ASSERT_TRUE(manager.ok()) << manager.status();
  EXPECT_EQ((*manager)->active_server_count(), 0);
  EXPECT_FALSE(std::filesystem::exists(marker));
  std::filesystem::remove(marker);
}

TEST(McpRuntimeTest, FailedStdioStartupDoesNotExposeTools) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor.ok());
  const std::string registry_path = TempRegistryPath();
  ASSERT_TRUE(SaveServerRegistry(registry_path, {MakeStdioEntry("broken", "/path/that/does/not/exist/server")}).ok());

  RuntimeOptions options;
  options.registry_path = registry_path;
  auto manager = StartMcpRuntime(&db, executor->get(), nullptr, options);

  ASSERT_TRUE(manager.ok()) << manager.status();
  EXPECT_EQ((*manager)->active_server_count(), 0);
  auto tools = db.GetTopLevelTools();
  ASSERT_TRUE(tools.ok());
  EXPECT_EQ(std::find_if(tools->begin(), tools->end(),
                         [](const Database::Tool& tool) { return tool.name == "mcp_broken_echo"; }),
            tools->end());
}

TEST(McpRuntimeTest, RefreshReplacesCatalogAtomicallyAndKeepsLastSnapshotOnFailure) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor.ok());
  FakeHttpClient http_client;
  const std::string registry_path = TempRegistryPath();
  ASSERT_TRUE(SaveServerRegistry(registry_path, {MakeEntry("github")}).ok());

  FakeRuntimeSession* session = nullptr;
  RuntimeOptions options;
  options.registry_path = registry_path;
  RuntimeManager manager(&db, executor->get(), &http_client, options,
                         [&session](const ServerRegistryEntry&, HttpClient*, const RuntimeOptions&) {
                           auto owned = std::make_unique<FakeRuntimeSession>(std::vector<Tool>{MakeTool("old")},
                                                                             ToolCallResult{});
                           session = owned.get();
                           return absl::StatusOr<std::unique_ptr<RuntimeSession>>(std::move(owned));
                         });
  ASSERT_TRUE(manager.Start().ok());
  ASSERT_NE(session, nullptr);

  session->SetTools({MakeTool("new")});
  ASSERT_TRUE(manager.RefreshCatalogs().ok());
  auto tools = db.GetTopLevelTools();
  ASSERT_TRUE(tools.ok());
  EXPECT_TRUE(std::any_of(tools->begin(), tools->end(),
                          [](const Database::Tool& tool) { return tool.name == "mcp_github_new"; }));
  EXPECT_TRUE(std::none_of(tools->begin(), tools->end(),
                           [](const Database::Tool& tool) { return tool.name == "mcp_github_old"; }));

  session->SetListStatus(absl::UnavailableError("temporary"));
  EXPECT_TRUE(manager.RefreshCatalogs().ok());
  tools = db.GetTopLevelTools();
  ASSERT_TRUE(tools.ok());
  EXPECT_TRUE(std::any_of(tools->begin(), tools->end(),
                          [](const Database::Tool& tool) { return tool.name == "mcp_github_new"; }));
}

TEST(McpRuntimeTest, ProviderUnsafeLongNamesAreRejected) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor.ok());
  FakeHttpClient http_client;

  const std::string registry_path = TempRegistryPath();
  ASSERT_TRUE(SaveServerRegistry(registry_path, {MakeEntry("github")}).ok());
  RuntimeOptions options;
  options.registry_path = registry_path;
  RuntimeManager manager(
      &db, executor->get(), &http_client, options, [](const ServerRegistryEntry&, HttpClient*, const RuntimeOptions&) {
        ToolCallResult result;
        result.content.push_back(nlohmann::json{{"type", "text"}, {"text", "ok"}});
        auto session = std::make_unique<FakeRuntimeSession>(
            std::vector<Tool>{MakeTool("tool_name_that_is_longer_than_provider_function_name_limits")}, result);
        return absl::StatusOr<std::unique_ptr<RuntimeSession>>(std::move(session));
      });
  absl::Status status = manager.Start();
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(absl::IsFailedPrecondition(status));
}

TEST(McpRuntimeTest, SanitizedToolNameCollisionsAreRejected) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor.ok());
  FakeHttpClient http_client;

  const std::string registry_path = TempRegistryPath();
  ASSERT_TRUE(SaveServerRegistry(registry_path, {MakeEntry("github")}).ok());
  RuntimeOptions options;
  options.registry_path = registry_path;
  RuntimeManager manager(&db, executor->get(), &http_client, options,
                         [](const ServerRegistryEntry&, HttpClient*, const RuntimeOptions&) {
                           ToolCallResult result;
                           result.content.push_back(nlohmann::json{{"type", "text"}, {"text", "ok"}});
                           auto session = std::make_unique<FakeRuntimeSession>(
                               std::vector<Tool>{MakeTool("search/repo"), MakeTool("search_repo")}, result);
                           return absl::StatusOr<std::unique_ptr<RuntimeSession>>(std::move(session));
                         });
  absl::Status status = manager.Start();
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(absl::IsFailedPrecondition(status));
  auto tools = db.GetTopLevelTools();
  ASSERT_TRUE(tools.ok());
  EXPECT_TRUE(std::none_of(tools->begin(), tools->end(),
                           [](const Database::Tool& tool) { return tool.name == "mcp_github_search_repo"; }));
  const auto names = executor->get()->GetRegisteredToolNamesForTest();
  EXPECT_EQ(std::count(names.begin(), names.end(), "mcp_github_search_repo"), 0);
}

TEST(McpRuntimeTest, ToolErrorIsPreservedInNormalizedOutput) {
  ToolCallResult result;
  result.is_error = true;
  result.content.push_back(nlohmann::json{{"type", "text"}, {"text", "tool failed"}});
  auto normalized = NormalizeToolCallResult(result);
  ASSERT_TRUE(normalized.ok()) << normalized.status();
  auto parsed = json_parse(*normalized);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE((*parsed)["is_error"].get<bool>());
  EXPECT_EQ((*parsed)["content"][0]["text"], "tool failed");
}

TEST(McpRuntimeTest, AuthFailureDoesNotExposeStaleTool) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.RegisterTool({"mcp_github_search", "stale", "{}", true, 0, true}).ok());
  auto executor = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor.ok());
  FakeHttpClient http_client;

  const std::string registry_path = TempRegistryPath();
  ASSERT_TRUE(SaveServerRegistry(registry_path, {MakeEntry("github")}).ok());
  RuntimeOptions options;
  options.registry_path = registry_path;
  RuntimeManager manager(
      &db, executor->get(), &http_client, options, [](const ServerRegistryEntry&, HttpClient*, const RuntimeOptions&) {
        return absl::StatusOr<std::unique_ptr<RuntimeSession>>(absl::UnauthenticatedError("login required"));
      });
  ASSERT_TRUE(manager.Start().ok());
  auto tools = db.GetTopLevelTools();
  ASSERT_TRUE(tools.ok());
  EXPECT_EQ(std::find_if(tools->begin(), tools->end(),
                         [](const Database::Tool& tool) { return tool.name == "mcp_github_search"; }),
            tools->end());
}

TEST(McpRuntimeTest, BearerTokenIsLoadedIntoTransportHeaders) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor.ok());
  FakeHttpClient http_client;

  ServerRegistryEntry entry = MakeEntry("github");
  entry.auth = kAuthBearer;
  entry.token_path =
      absl::StrCat(::testing::TempDir(), "/std_slop_mcp_runtime_token_", absl::ToUnixNanos(absl::Now()), ".json");
  OAuthTokenSet tokens;
  tokens.access_token = "secret-token";
  ASSERT_TRUE(SaveOAuthTokens(entry.token_path, tokens).ok());
  const std::string registry_path = TempRegistryPath();
  ASSERT_TRUE(SaveServerRegistry(registry_path, {entry}).ok());

  RuntimeOptions options;
  options.registry_path = registry_path;
  auto manager = StartMcpRuntime(&db, executor->get(), &http_client, options);

  ASSERT_TRUE(manager.ok()) << manager.status();
  EXPECT_TRUE(HasHeader(http_client.last_headers, "Authorization: Bearer secret-token"));
  auto tools = db.GetTopLevelTools();
  ASSERT_TRUE(tools.ok());
  EXPECT_NE(std::find_if(tools->begin(), tools->end(),
                         [](const Database::Tool& tool) { return tool.name == "mcp_github_search"; }),
            tools->end());
}

TEST(McpRuntimeTest, MissingBearerTokenDoesNotExposeStaleTool) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.RegisterTool({"mcp_github_search", "stale", "{}", true, 0, true}).ok());
  auto executor = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor.ok());
  FakeHttpClient http_client;

  ServerRegistryEntry entry = MakeEntry("github");
  entry.auth = kAuthBearer;
  entry.token_path =
      absl::StrCat(::testing::TempDir(), "/missing_mcp_runtime_token_", absl::ToUnixNanos(absl::Now()), ".json");
  const std::string registry_path = TempRegistryPath();
  ASSERT_TRUE(SaveServerRegistry(registry_path, {entry}).ok());
  RuntimeOptions options;
  options.registry_path = registry_path;

  auto manager = StartMcpRuntime(&db, executor->get(), &http_client, options);

  ASSERT_TRUE(manager.ok()) << manager.status();
  EXPECT_EQ((*manager)->active_server_count(), 0);
  EXPECT_EQ(http_client.request_count, 0);
  auto tools = db.GetTopLevelTools();
  ASSERT_TRUE(tools.ok());
  EXPECT_EQ(std::find_if(tools->begin(), tools->end(),
                         [](const Database::Tool& tool) { return tool.name == "mcp_github_search"; }),
            tools->end());
}

TEST(McpRuntimeTest, InvalidBearerTokenDoesNotStartServer) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor.ok());
  FakeHttpClient http_client;

  ServerRegistryEntry entry = MakeEntry("github");
  entry.auth = kAuthBearer;
  entry.token_path =
      absl::StrCat(::testing::TempDir(), "/invalid_mcp_runtime_token_", absl::ToUnixNanos(absl::Now()), ".json");
  {
    std::ofstream token_file(entry.token_path);
    token_file << "not json";
  }
  const std::string registry_path = TempRegistryPath();
  ASSERT_TRUE(SaveServerRegistry(registry_path, {entry}).ok());
  RuntimeOptions options;
  options.registry_path = registry_path;

  auto manager = StartMcpRuntime(&db, executor->get(), &http_client, options);

  ASSERT_TRUE(manager.ok()) << manager.status();
  EXPECT_EQ((*manager)->active_server_count(), 0);
  EXPECT_EQ(http_client.request_count, 0);
}

TEST(McpRuntimeTest, ToolCallAuthFailureIncludesBearerHint) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor.ok());
  FakeHttpClient http_client;

  ServerRegistryEntry entry = MakeEntry("github");
  entry.auth = kAuthBearer;
  entry.token_path =
      absl::StrCat(::testing::TempDir(), "/std_slop_mcp_runtime_tool_token_", absl::ToUnixNanos(absl::Now()), ".json");
  const std::string registry_path = TempRegistryPath();
  ASSERT_TRUE(SaveServerRegistry(registry_path, {entry}).ok());
  RuntimeOptions options;
  options.registry_path = registry_path;
  RuntimeManager manager(
      &db, executor->get(), &http_client, options, [](const ServerRegistryEntry&, HttpClient*, const RuntimeOptions&) {
        auto session =
            std::make_unique<FakeRuntimeSession>(std::vector<Tool>{MakeTool("search")}, ToolCallResult{},
                                                 absl::UnauthenticatedError("server requires authentication"));
        return absl::StatusOr<std::unique_ptr<RuntimeSession>>(std::move(session));
      });
  ASSERT_TRUE(manager.Start().ok());
  auto call = (*executor)->Execute("mcp_github_search", nlohmann::json{{"query", "repo"}});
  ASSERT_FALSE(call.ok());
  EXPECT_TRUE(absl::IsUnauthenticated(call.status()));
  EXPECT_NE(std::string(call.status().message())
                .find("std_slop mcp add github --url https://github.example/mcp --auth bearer --token <token>"),
            std::string::npos);
  EXPECT_EQ(std::string(call.status().message()).find("secret-token"), std::string::npos);
}

TEST(McpRuntimeTest, MalformedResultContentIsRejected) {
  ToolCallResult result;
  result.content.push_back("not an object");
  auto normalized = NormalizeToolCallResult(result);
  EXPECT_FALSE(normalized.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(normalized.status()));
}

TEST(McpRuntimeTest, ScalarStructuredContentIsPreserved) {
  ToolCallResult result;
  result.content.push_back(nlohmann::json{{"type", "text"}, {"text", "ok"}});
  result.structured_content = "scalar";
  auto normalized = NormalizeToolCallResult(result);
  ASSERT_TRUE(normalized.ok()) << normalized.status();
  auto parsed = json_parse(*normalized);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ((*parsed)["structured_content"], "scalar");
}

}  // namespace
}  // namespace slop::mcp
