#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "core/json_utils.h"
#include "mcp/gateway/broker.h"
#include "mcp/gateway/catalog.h"
#include "mcp/gateway/server.h"
#include "mcp/protocol.h"

namespace slop::mcp::gateway {
namespace {

class EchoClient final : public Client {
 public:
  ProtocolRevision revision() const override { return ProtocolRevision::k2026_07_28; }
  absl::StatusOr<std::vector<Tool>> ListTools() override {
    Tool tool;
    tool.name = "echo";
    tool.description = "Echo one text field";
    tool.input_schema = {{"type", "object"},
                         {"properties", {{"text", {{"type", "string"}}}}},
                         {"required", {"text"}},
                         {"additionalProperties", false}};
    return std::vector<Tool>{std::move(tool)};
  }
  absl::StatusOr<ToolCallResult> CallTool(const std::string& name, const nlohmann::json& arguments,
                                          absl::Duration) override {
    ++calls;
    if (name != "echo") return absl::NotFoundError("missing tool");
    ToolCallResult result;
    result.content.push_back({{"type", "text"}, {"text", json_get_or(arguments, "text", std::string())}});
    result.structured_content = arguments;
    return result;
  }
  absl::StatusOr<ToolCallResult> ContinueToolCall(const std::string&, const nlohmann::json&, const nlohmann::json&,
                                                  absl::Duration) override {
    return absl::UnimplementedError("not supported");
  }
  int calls = 0;
};

std::shared_ptr<SerialBroker> MakeBroker(EchoClient** client_out = nullptr) {
  auto client = std::make_unique<EchoClient>();
  if (client_out != nullptr) *client_out = client.get();
  std::vector<DownstreamClient> downstreams;
  downstreams.push_back({"echo", {"echo"}, std::move(client)});
  auto catalog = Catalog::Create(std::move(downstreams));
  if (!catalog.ok()) return nullptr;
  return std::make_shared<SerialBroker>(std::move(*catalog));
}

nlohmann::json Request(const std::string& tool, const nlohmann::json& arguments, const std::string& id = "1") {
  return {{"jsonrpc", "2.0"},
          {"id", id},
          {"method", "tools/call"},
          {"params",
           {{"name", tool},
            {"arguments", arguments},
            {"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}}}}}};
}

std::optional<nlohmann::json> Call(const server::Server& server, const std::string& tool,
                                   const nlohmann::json& arguments, const std::string& id = "1") {
  return server.Dispatch(json_dump(Request(tool, arguments, id)));
}

TEST(GatewayTest, DiscoversAndRunsNamespacedAsyncTool) {
  EchoClient* client = nullptr;
  auto broker = MakeBroker(&client);
  ASSERT_NE(broker, nullptr);
  auto server = CreateServer(broker);
  ASSERT_TRUE(server.ok()) << server.status();
  auto listed = server->Dispatch(
      json_dump({{"jsonrpc", "2.0"},
                 {"id", "list"},
                 {"method", "tools/list"},
                 {"params", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}}}}}}));
  ASSERT_TRUE(listed.has_value());
  ASSERT_EQ((*listed)["result"]["tools"].size(), 2);

  auto reply = Call(*server, "run_js",
                    {{"code",
                      "const first = await echo.echo({text: input.text}); "
                      "const second = await mcp.call('echo', 'echo', {text: first.structuredContent.text}); "
                      "return second.structuredContent;"},
                     {"input", {{"text", "hello"}}}});
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ((*reply)["result"]["structuredContent"]["result"]["text"], "hello") << json_dump(*reply);
  EXPECT_EQ(client->calls, 2);
}

TEST(GatewayTest, HelpWorksInsideAndOutsideTheScript) {
  auto broker = MakeBroker();
  auto server = CreateServer(broker);
  ASSERT_TRUE(server.ok()) << server.status();
  auto help = Call(*server, "run_js_help", nlohmann::json::object());
  ASSERT_TRUE(help.has_value());
  EXPECT_EQ((*help)["result"]["structuredContent"]["tools"].size(), 1);

  auto missing = Call(*server, "run_js_help", {{"server", "echo"}, {"tool", "missing"}});
  ASSERT_TRUE(missing.has_value());
  EXPECT_EQ((*missing)["result"]["isError"], true);
  EXPECT_EQ((*missing)["result"]["structuredContent"]["error"]["category"], "catalog");

  auto reply = Call(*server, "run_js", {{"code", "return help('echo', 'echo');"}});
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ((*reply)["result"]["structuredContent"]["result"]["tools"][0]["name"], "echo") << json_dump(*reply);
  EXPECT_TRUE((*reply)["result"]["structuredContent"]["result"]["tools"][0].contains("inputSchema"));
}

TEST(GatewayTest, DeniedToolsAndInvalidArgumentsDoNotReachDownstream) {
  EchoClient* client = nullptr;
  auto broker = MakeBroker(&client);
  auto server = CreateServer(broker);
  ASSERT_TRUE(server.ok()) << server.status();

  auto denied = Call(*server, "run_js", {{"code", "await mcp.call('echo', 'hidden', {}); return 1;"}});
  ASSERT_TRUE(denied.has_value());
  EXPECT_EQ((*denied)["result"]["isError"], true) << json_dump(*denied);
  EXPECT_EQ(client->calls, 0);

  auto invalid = Call(*server, "run_js", {{"code", "await echo.echo({text: 1}); return 1;"}});
  ASSERT_TRUE(invalid.has_value());
  EXPECT_EQ((*invalid)["result"]["isError"], true) << json_dump(*invalid);
  EXPECT_EQ(client->calls, 0);
}

}  // namespace
}  // namespace slop::mcp::gateway
