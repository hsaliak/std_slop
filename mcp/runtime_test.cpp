#include "mcp/runtime.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "core/json_utils.h"
#include "core/http_client.h"
#include "mcp/protocol.h"
#include "mcp/registry.h"
#include "mcp/token_store.h"
#include "nlohmann/json.hpp"
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
    nlohmann::json response = {{"jsonrpc", "2.0"}, {"id", json_get_or(*request, "id", 0)}};
    if (method == "initialize") {
      response["result"] = {{"protocolVersion", std::string(kLatestProtocolVersion)},
                            {"capabilities", nlohmann::json::object()}};
    } else if (method == "tools/list") {
      response["result"] = {{"tools", nlohmann::json::array({{{"name", "search"},
                                                                 {"description", "Search"},
                                                                 {"inputSchema", {{"type", "object"}}}}})}};
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

  std::string last_url;
  std::vector<std::string> last_headers;
  int request_count = 0;
};

class FakeRuntimeSession : public RuntimeSession {
 public:
  FakeRuntimeSession(std::vector<Tool> tools, ToolCallResult result, absl::Status call_status = absl::OkStatus())
      : tools_(std::move(tools)), result_(std::move(result)), call_status_(std::move(call_status)) {}

  absl::StatusOr<std::vector<Tool>> ListTools() override { return tools_; }

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
                           auto session = std::make_unique<FakeRuntimeSession>(std::vector<Tool>{MakeTool("search")}, result);
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
  RuntimeManager manager(&db, executor->get(), &http_client, options,
                         [](const ServerRegistryEntry&, HttpClient*, const RuntimeOptions&) {
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
  RuntimeManager manager(&db, executor->get(), &http_client, options,
                         [](const ServerRegistryEntry&, HttpClient*, const RuntimeOptions&) {
                           return absl::StatusOr<std::unique_ptr<RuntimeSession>>(
                               absl::UnauthenticatedError("login required"));
                         });
  ASSERT_TRUE(manager.Start().ok());
  auto tools = db.GetTopLevelTools();
  ASSERT_TRUE(tools.ok());
  EXPECT_EQ(std::find_if(tools->begin(), tools->end(), [](const Database::Tool& tool) {
              return tool.name == "mcp_github_search";
            }),
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
  entry.token_path = absl::StrCat(::testing::TempDir(), "/std_slop_mcp_runtime_token_", absl::ToUnixNanos(absl::Now()), ".json");
  ASSERT_TRUE(SaveOAuthTokens(entry.token_path, {"secret-token", "", 0}).ok());
  const std::string registry_path = TempRegistryPath();
  ASSERT_TRUE(SaveServerRegistry(registry_path, {entry}).ok());

  RuntimeOptions options;
  options.registry_path = registry_path;
  auto manager = StartMcpRuntime(&db, executor->get(), &http_client, options);

  ASSERT_TRUE(manager.ok()) << manager.status();
  EXPECT_TRUE(HasHeader(http_client.last_headers, "Authorization: Bearer secret-token"));
  auto tools = db.GetTopLevelTools();
  ASSERT_TRUE(tools.ok());
  EXPECT_NE(std::find_if(tools->begin(), tools->end(), [](const Database::Tool& tool) {
              return tool.name == "mcp_github_search";
            }),
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
  entry.token_path = absl::StrCat(::testing::TempDir(), "/missing_mcp_runtime_token_", absl::ToUnixNanos(absl::Now()), ".json");
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
  EXPECT_EQ(std::find_if(tools->begin(), tools->end(), [](const Database::Tool& tool) {
              return tool.name == "mcp_github_search";
            }),
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
  entry.token_path = absl::StrCat(::testing::TempDir(), "/invalid_mcp_runtime_token_", absl::ToUnixNanos(absl::Now()), ".json");
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
  entry.token_path = absl::StrCat(::testing::TempDir(), "/std_slop_mcp_runtime_tool_token_", absl::ToUnixNanos(absl::Now()), ".json");
  const std::string registry_path = TempRegistryPath();
  ASSERT_TRUE(SaveServerRegistry(registry_path, {entry}).ok());
  RuntimeOptions options;
  options.registry_path = registry_path;
  RuntimeManager manager(&db, executor->get(), &http_client, options,
                         [](const ServerRegistryEntry&, HttpClient*, const RuntimeOptions&) {
                           auto session = std::make_unique<FakeRuntimeSession>(
                               std::vector<Tool>{MakeTool("search")}, ToolCallResult{},
                               absl::UnauthenticatedError("server requires authentication"));
                           return absl::StatusOr<std::unique_ptr<RuntimeSession>>(std::move(session));
                         });
  ASSERT_TRUE(manager.Start().ok());
  auto call = (*executor)->Execute("mcp_github_search", nlohmann::json{{"query", "repo"}});
  ASSERT_FALSE(call.ok());
  EXPECT_TRUE(absl::IsUnauthenticated(call.status()));
  EXPECT_NE(std::string(call.status().message()).find("std_slop mcp add github --url https://github.example/mcp --auth bearer --token <token>"),
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

TEST(McpRuntimeTest, MalformedStructuredContentIsRejected) {
  ToolCallResult result;
  result.content.push_back(nlohmann::json{{"type", "text"}, {"text", "ok"}});
  result.structured_content = "not an object";
  auto normalized = NormalizeToolCallResult(result);
  EXPECT_FALSE(normalized.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(normalized.status()));
}

}  // namespace
}  // namespace slop::mcp
