#include "mcp/local_tools/server.h"

#include <string>

#include "core/json_utils.h"
#include "mcp/local_tools/workspace_catalog.h"

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

TEST(LocalToolsServerTest, WorkspaceListAndInfoExposeLaunchDefault) {
  auto catalog = WorkspaceCatalog::Create();
  ASSERT_TRUE(catalog.ok()) << catalog.status().message();
  auto launch_workspace = catalog->Resolve("launch");
  ASSERT_TRUE(launch_workspace.ok()) << launch_workspace.status().message();
  const std::string launch_root = launch_workspace->root.string();

  auto server = CreateServer(std::move(*catalog));
  ASSERT_TRUE(server.ok()) << server.status().message();

  const nlohmann::json list_response = CallTool(*server, 1, "workspace_list", nlohmann::json::object());
  ASSERT_TRUE(list_response.contains("result"));
  const auto& list_result = list_response["result"];
  EXPECT_FALSE(list_result["isError"].get<bool>());
  const auto& list = list_result["structuredContent"];
  EXPECT_EQ(list["default"], "launch");
  ASSERT_GT(list["workspaces"].size(), 0);
  EXPECT_EQ(list["workspaces"][0]["id"], "launch");
  EXPECT_EQ(list["workspaces"][0]["root"], launch_root);
  EXPECT_TRUE(list["workspaces"][0]["is_default"].get<bool>());

  const nlohmann::json info_response = CallTool(*server, 2, "workspace_info", nlohmann::json::object());
  ASSERT_TRUE(info_response.contains("result"));
  const auto& info_result = info_response["result"];
  EXPECT_FALSE(info_result["isError"].get<bool>());
  EXPECT_EQ(info_result["structuredContent"]["id"], "launch");
  EXPECT_EQ(info_result["structuredContent"]["root"], launch_root);
}

TEST(LocalToolsServerTest, UnknownWorkspaceReturnsToolError) {
  auto catalog = WorkspaceCatalog::Create();
  ASSERT_TRUE(catalog.ok()) << catalog.status().message();
  auto server = CreateServer(std::move(*catalog));
  ASSERT_TRUE(server.ok()) << server.status().message();

  const nlohmann::json response = CallTool(*server, 1, "workspace_info", {{"workspace", "worktree:/missing"}});
  ASSERT_TRUE(response.contains("result"));
  EXPECT_TRUE(response["result"]["isError"].get<bool>());
  EXPECT_EQ(response["result"]["structuredContent"]["error"]["message"],
            "Workspace is unavailable: worktree:/missing");
}

TEST(LocalToolsServerTest, WorkspaceInfoRejectsWrongArgumentShape) {
  auto catalog = WorkspaceCatalog::Create();
  ASSERT_TRUE(catalog.ok()) << catalog.status().message();
  auto server = CreateServer(std::move(*catalog));
  ASSERT_TRUE(server.ok()) << server.status().message();

  const nlohmann::json response = CallTool(*server, 1, "workspace_info", {{"workspace", 42}});
  ASSERT_TRUE(response.contains("result"));
  EXPECT_TRUE(response["result"]["isError"].get<bool>());
}

}  // namespace
}  // namespace slop::mcp::local_tools
