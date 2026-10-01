#include "mcp/server/server.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "core/json_utils.h"
#include "mcp/client/modern.h"
#include "mcp/json_rpc.h"
#include "mcp/protocol.h"

namespace slop::mcp::server {
namespace {

nlohmann::json Request() {
  return {{"jsonrpc", "2.0"},
          {"id", "discover-1"},
          {"method", "server/discover"},
          {"params", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}}}}}};
}

Server MakeServer() {
  ImplementationInfo identity;
  identity.name = "example";
  identity.version = "1.0";
  identity.title = "Example server";
  return *Server::Create(identity);
}

int ErrorCode(const Server& server, const nlohmann::json& request) {
  const auto reply = server.Dispatch(json_dump(request));
  EXPECT_TRUE(reply.has_value());
  if (!reply) return 0;
  EXPECT_TRUE(ParseJsonRpcResponse(*reply).ok());
  return json_get_or((*reply)["error"], "code", 0);
}

TEST(ServerTest, ValidatesIdentity) {
  EXPECT_FALSE(Server::Create({}).ok());
  ImplementationInfo identity;
  identity.name = "example";
  EXPECT_FALSE(Server::Create(identity).ok());
  identity.version = "1";
  EXPECT_TRUE(Server::Create(identity).ok());
}

TEST(ServerTest, DiscoveryIsUsableByExistingModernCodec) {
  const Server server = MakeServer();
  const auto reply = server.Dispatch(json_dump(Request()));
  ASSERT_TRUE(reply);
  EXPECT_EQ((*reply)["id"], "discover-1");
  EXPECT_EQ((*reply)["result"]["resultType"], "complete");
  auto discovery = v2026_07_28::ParseDiscovery((*reply)["result"]);
  ASSERT_TRUE(discovery.ok()) << discovery.status();
  EXPECT_EQ(discovery->supported_versions, std::vector<std::string>{std::string(kModernProtocolVersion)});
  EXPECT_TRUE(discovery->capabilities.empty());
  ASSERT_TRUE(discovery->server_info);
  EXPECT_EQ(discovery->server_info->name, "example");
  EXPECT_EQ(discovery->server_info->title, "Example server");
}

TEST(ServerTest, DeclinesClassicInitializeAndRemainsUsable) {
  const Server server = MakeServer();
  // Match v2025_11_25::Session::Initialize: classic clients put the version
  // in params.protocolVersion, not modern per-request metadata.
  const auto request = BuildJsonRpcRequest(int64_t{44}, "initialize",
                                           {{"protocolVersion", std::string(kClassicProtocolVersion)},
                                            {"capabilities", nlohmann::json::object()},
                                            {"clientInfo", {{"name", "classic-client"}, {"version", "1.0"}}}});
  const auto reply = server.Dispatch(json_dump(request));
  ASSERT_TRUE(reply);
  auto response = ParseJsonRpcResponse(*reply);
  ASSERT_TRUE(response.ok()) << response.status();
  EXPECT_EQ(response->id, JsonRpcId(int64_t{44}));
  EXPECT_FALSE(response->result);
  ASSERT_TRUE(response->error);
  EXPECT_EQ(response->error->code, -32602);
  EXPECT_FALSE(response->error->message.empty());

  // An old initialized notification does not select a legacy mode or get a reply.
  EXPECT_FALSE(server.Dispatch(json_dump(BuildJsonRpcNotification("notifications/initialized"))));
  const auto modern_reply = server.Dispatch(json_dump(Request()));
  ASSERT_TRUE(modern_reply);
  EXPECT_TRUE(v2026_07_28::ParseDiscovery((*modern_reply)["result"]).ok());
}

TEST(ServerTest, AcceptsActualClientEncodedRequest) {
  v2026_07_28::Request request;
  request.id = int64_t{42};
  request.method = "server/discover";
  request.context.client_info.name = "client";
  request.context.client_info.version = "1";
  auto encoded = v2026_07_28::EncodeRequest(request);
  ASSERT_TRUE(encoded.ok());
  auto reply = MakeServer().Dispatch(json_dump(encoded->body));
  ASSERT_TRUE(reply);
  EXPECT_EQ((*reply)["id"], 42);
  EXPECT_TRUE(reply->contains("result"));
}

TEST(ServerTest, MalformedInputAndInvalidEnvelope) {
  const Server server = MakeServer();
  for (const std::string raw : {"", "{", "not-json"}) {
    auto reply = server.Dispatch(raw);
    ASSERT_TRUE(reply);
    EXPECT_EQ((*reply)["error"]["code"], -32700);
    EXPECT_TRUE((*reply)["id"].is_null());
  }
  for (const auto& value : {nlohmann::json(nullptr), nlohmann::json::array(), nlohmann::json(5)}) {
    EXPECT_EQ(ErrorCode(server, value), -32600);
  }
  for (const auto& id : {nlohmann::json(nullptr), nlohmann::json(true), nlohmann::json(1.5),
                         nlohmann::json(9007199254740992ULL), nlohmann::json(-9007199254740992LL)}) {
    auto request = Request();
    request["id"] = id;
    EXPECT_EQ(ErrorCode(server, request), -32600);
  }
  for (const std::string field : {"result", "error"}) {
    auto request = Request();
    request[field] = nlohmann::json::object();
    EXPECT_EQ(ErrorCode(server, request), -32600);
  }
  auto request = Request();
  request["jsonrpc"] = "1.0";
  EXPECT_EQ(ErrorCode(server, request), -32600);
  request = Request();
  request.erase("method");
  EXPECT_EQ(ErrorCode(server, request), -32600);
}

TEST(ServerTest, ValidatesMetadataAndVersionBeforeDispatch) {
  const Server server = MakeServer();
  for (const auto& params : {nlohmann::json(nullptr), nlohmann::json::array(), nlohmann::json::object(),
                             nlohmann::json{{"_meta", nullptr}}}) {
    auto request = Request();
    request["params"] = params;
    EXPECT_EQ(ErrorCode(server, request), -32602);
  }
  for (const auto& version : {nlohmann::json(nullptr), nlohmann::json(5), nlohmann::json("")}) {
    auto request = Request();
    request["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = version;
    EXPECT_EQ(ErrorCode(server, request), -32602);
  }
  auto request = Request();
  request["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2025-11-25";
  EXPECT_EQ(ErrorCode(server, request), -32022);
  auto reply = server.Dispatch(json_dump(request));
  ASSERT_TRUE(reply);
  EXPECT_EQ((*reply)["id"], "discover-1");
  EXPECT_EQ((*reply)["error"]["data"]["supported"], nlohmann::json::array({"2026-07-28"}));
  EXPECT_EQ((*reply)["error"]["data"]["requested"], "2025-11-25");
}

TEST(ServerTest, RejectsMalformedOptionalClientMetadata) {
  const Server server = MakeServer();
  for (const auto& info :
       {nlohmann::json(nullptr), nlohmann::json::object(), nlohmann::json{{"name", "client"}, {"version", 1}}}) {
    auto request = Request();
    request["params"]["_meta"]["io.modelcontextprotocol/clientInfo"] = info;
    EXPECT_EQ(ErrorCode(server, request), -32602);
  }
  auto request = Request();
  request["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"] = false;
  EXPECT_EQ(ErrorCode(server, request), -32602);
}

TEST(ServerTest, UnknownMethodsAndNotifications) {
  const Server server = MakeServer();
  for (const std::string method : {"initialize", "tools/list", "tools/call", "unknown"}) {
    auto request = Request();
    request["method"] = method;
    EXPECT_EQ(ErrorCode(server, request), -32601);
    request.erase("id");
    EXPECT_FALSE(server.Dispatch(json_dump(request)));
  }
}

TEST(ServerTest, SafeIntegerBoundaryIdsArePreserved) {
  const Server server = MakeServer();
  for (const auto& id : {nlohmann::json(9007199254740991ULL), nlohmann::json(-9007199254740991LL), nlohmann::json(0),
                         nlohmann::json("")}) {
    auto request = Request();
    request["id"] = id;
    auto reply = server.Dispatch(json_dump(request));
    ASSERT_TRUE(reply);
    EXPECT_EQ((*reply)["id"], id);
    EXPECT_TRUE(reply->contains("result"));
  }
}

}  // namespace
}  // namespace slop::mcp::server
