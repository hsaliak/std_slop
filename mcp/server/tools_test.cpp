#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "gtest/gtest.h"

#include "core/json_utils.h"
#include "mcp/client/modern.h"
#include "mcp/json_rpc.h"
#include "mcp/server/server.h"

namespace slop::mcp::server {
namespace {

ImplementationInfo Identity() {
  ImplementationInfo identity;
  identity.name = "tools-test";
  identity.version = "1";
  return identity;
}

ToolRegistration Echo(int* calls) {
  ToolRegistration registration;
  registration.definition.name = "echo";
  registration.definition.title = "Echo";
  registration.definition.description = "Return text";
  registration.definition.input_schema = {{"type", "object"},
                                          {"properties", {{"text", {{"type", "string"}}}}},
                                          {"required", {"text"}},
                                          {"additionalProperties", false}};
  registration.definition.output_schema = registration.definition.input_schema;
  registration.definition.annotations = {{"readOnlyHint", true}};
  registration.definition.meta = {{"example", true}};
  registration.handler = [calls](const nlohmann::json& arguments) -> absl::StatusOr<ToolCallResult> {
    ++*calls;
    ToolCallResult result;
    result.content.push_back({{"type", "text"}, {"text", json_get_or(arguments, "text", std::string{})}});
    result.structured_content = arguments;
    result.meta = {{"example", true}};
    return result;
  };
  return registration;
}

nlohmann::json Request(const std::string& method = "tools/call") {
  return {{"jsonrpc", "2.0"},
          {"id", "call-1"},
          {"method", method},
          {"params", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}}}}}};
}

nlohmann::json Call() {
  auto request = Request();
  request["params"]["name"] = "echo";
  request["params"]["arguments"] = {{"text", "hello"}};
  return request;
}

nlohmann::json Reply(const Server& server, const nlohmann::json& request) {
  const auto response = server.Dispatch(json_dump(request));
  EXPECT_TRUE(response);
  if (!response) return nullptr;
  EXPECT_TRUE(ParseJsonRpcResponse(*response).ok());
  EXPECT_EQ((*response)["id"], request["id"]);
  return *response;
}

TEST(ServerToolsTest, SortedFrozenCatalogAndWireCompatibility) {
  int calls = 0;
  auto echo = Echo(&calls);
  auto other = echo;
  other.definition.name = "aaa";
  auto server = Server::Create(Identity(), {echo, other});
  ASSERT_TRUE(server.ok()) << server.status();
  echo.definition.title = "Changed after registration";
  auto reply = Reply(*server, Request("tools/list"));
  EXPECT_FALSE(reply["result"].contains("nextCursor"));
  auto tools = v2026_07_28::ParseToolsList(reply["result"]);
  ASSERT_TRUE(tools.ok()) << tools.status();
  ASSERT_EQ(tools->size(), 2);
  EXPECT_EQ((*tools)[0].name, "aaa");
  EXPECT_EQ((*tools)[1].name, "echo");
  EXPECT_EQ((*tools)[1].title, "Echo");
  EXPECT_EQ((*tools)[1].description, "Return text");
  EXPECT_EQ((*tools)[1].input_schema, other.definition.input_schema);
  EXPECT_EQ((*tools)[1].output_schema, other.definition.output_schema);
  EXPECT_EQ((*tools)[1].annotations, other.definition.annotations);
  EXPECT_EQ((*tools)[1].meta, other.definition.meta);
  EXPECT_EQ(calls, 0);
}

TEST(ServerToolsTest, EmptyCatalogAndUnsupportedCursor) {
  auto server = Server::Create(Identity());
  ASSERT_TRUE(server.ok());
  auto request = Request("tools/list");
  EXPECT_EQ(Reply(*server, request)["result"]["tools"], nlohmann::json::array());
  for (const auto& cursor : {nlohmann::json(nullptr), nlohmann::json("next"), nlohmann::json(4)}) {
    request["params"]["cursor"] = cursor;
    EXPECT_EQ(Reply(*server, request)["error"]["code"], -32602);
  }
}

TEST(ServerToolsTest, ValidatesNamesHandlersAndDuplicates) {
  int calls = 0;
  for (const std::string& name :
       std::vector<std::string>{"", "has space", "comma,", "slash/", "non-ascii\xc3\xa9", std::string(129, 'a')}) {
    auto tool = Echo(&calls);
    tool.definition.name = name;
    EXPECT_FALSE(Server::Create(Identity(), {tool}).ok());
  }
  for (const std::string& name : {std::string("Admin.tools_v2-1"), std::string(128, 'a')}) {
    auto tool = Echo(&calls);
    tool.definition.name = name;
    EXPECT_TRUE(Server::Create(Identity(), {tool}).ok());
  }
  auto tool = Echo(&calls);
  EXPECT_FALSE(Server::Create(Identity(), {tool, tool}).ok());
  tool.handler = {};
  EXPECT_FALSE(Server::Create(Identity(), {tool}).ok());
  EXPECT_EQ(calls, 0);
}

TEST(ServerToolsTest, ValidatesSchemasAndMetadataAtCreation) {
  int calls = 0;
  for (const auto& schema :
       {nlohmann::json(nullptr), nlohmann::json(false), nlohmann::json{{"type", 5}}, nlohmann::json{{"type", "array"}},
        nlohmann::json{{"$ref", "https://example.com/schema"}}}) {
    auto tool = Echo(&calls);
    tool.definition.input_schema = schema;
    EXPECT_FALSE(Server::Create(Identity(), {tool}).ok());
  }
  for (const auto& schema : {nlohmann::json(nullptr), nlohmann::json(false), nlohmann::json{{"type", "invalid"}}}) {
    auto tool = Echo(&calls);
    tool.definition.output_schema = schema;
    EXPECT_FALSE(Server::Create(Identity(), {tool}).ok());
  }
  for (const bool annotations : {false, true}) {
    auto tool = Echo(&calls);
    if (annotations)
      tool.definition.annotations = nullptr;
    else
      tool.definition.meta = nullptr;
    EXPECT_FALSE(Server::Create(Identity(), {tool}).ok());
  }
  EXPECT_EQ(calls, 0);
}

TEST(ServerToolsTest, RejectsEmptySchemaArraysAtRegistration) {
  int calls = 0;
  for (const std::string keyword : {"allOf", "anyOf", "oneOf", "prefixItems"}) {
    const nlohmann::json empty = {{keyword, nlohmann::json::array()}};
    for (const auto& schema : {empty, nlohmann::json{{"properties", {{"value", empty}}}}}) {
      auto tool = Echo(&calls);
      tool.definition.input_schema = schema;
      EXPECT_FALSE(Server::Create(Identity(), {tool}).ok());
      tool = Echo(&calls);
      tool.definition.output_schema = schema;
      EXPECT_FALSE(Server::Create(Identity(), {tool}).ok());
    }
  }
  EXPECT_EQ(calls, 0);
}

TEST(ServerToolsTest, ExhaustedNegatedSchemaCannotExecuteOrProduceSuccess) {
  const nlohmann::json schema = {
      {"type", "object"},
      {"not", {{"$ref", "#/$defs/node"}}},
      {"$defs", {{"node", {{"type", "object"}, {"properties", {{"next", {{"$ref", "#/$defs/node"}}}}}}}}}};
  nlohmann::json deep = nlohmann::json::object();
  for (int i = 0; i < 40; ++i) deep = {{"next", std::move(deep)}};
  int calls = 0;
  auto input_tool = Echo(&calls);
  input_tool.definition.input_schema = schema;
  input_tool.definition.output_schema = nlohmann::json::object();
  auto input_server = Server::Create(Identity(), {input_tool});
  ASSERT_TRUE(input_server.ok());
  for (const auto& arguments : {nlohmann::json::object(), deep}) {
    auto request = Call();
    request["params"]["arguments"] = arguments;
    EXPECT_EQ(Reply(*input_server, request)["result"]["isError"], true);
    EXPECT_EQ(calls, 0);
  }

  calls = 0;
  auto output_tool = Echo(&calls);
  output_tool.definition.output_schema = schema;
  output_tool.handler = [&calls, &deep](const nlohmann::json&) -> absl::StatusOr<ToolCallResult> {
    ++calls;
    ToolCallResult result;
    result.structured_content = deep;
    return result;
  };
  auto output_server = Server::Create(Identity(), {output_tool});
  ASSERT_TRUE(output_server.ok());
  EXPECT_EQ(Reply(*output_server, Call())["error"]["code"], -32603);
  EXPECT_EQ(calls, 1);
}

TEST(ServerToolsTest, UnicodeLengthLimitsGateCallsAndResults) {
  int calls = 0;
  auto tool = Echo(&calls);
  tool.definition.input_schema["properties"]["text"] = {{"type", "string"}, {"minLength", 1}, {"maxLength", 1}};
  tool.definition.output_schema = tool.definition.input_schema;
  auto server = Server::Create(Identity(), {tool});
  ASSERT_TRUE(server.ok());
  for (const std::string text : {"a", "\xc3\xa9", "\xe2\x98\x83", "\xf0\x9f\x98\x80"}) {
    auto request = Call();
    request["params"]["arguments"]["text"] = text;
    const auto reply = Reply(*server, request);
    ASSERT_EQ(reply["result"]["isError"], false);
    EXPECT_EQ(reply["result"]["structuredContent"]["text"], text);
  }
  EXPECT_EQ(calls, 4);
  auto request = Call();
  request["params"]["arguments"]["text"] = "e\xcc\x81";
  EXPECT_EQ(Reply(*server, request)["result"]["isError"], true);
  EXPECT_EQ(calls, 4);
}

TEST(ServerToolsTest, CallsToolAndValidatesStructuredResult) {
  int calls = 0;
  auto tool = Echo(&calls);
  auto server = Server::Create(Identity(), {tool});
  ASSERT_TRUE(server.ok());
  const auto reply = Reply(*server, Call());
  EXPECT_EQ(reply["result"]["resultType"], "complete");
  auto result = v2026_07_28::ParseToolCallResult(reply["result"], &tool.definition.output_schema);
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_FALSE(result->is_error);
  ASSERT_TRUE(result->structured_content);
  EXPECT_EQ(*result->structured_content, nlohmann::json({{"text", "hello"}}));
  EXPECT_EQ(result->content.front()["text"], "hello");
  EXPECT_EQ(result->meta, nlohmann::json({{"example", true}}));
  EXPECT_EQ(calls, 1);
}

TEST(ServerToolsTest, InvalidToolCallsCannotExecute) {
  int calls = 0;
  auto server = Server::Create(Identity(), {Echo(&calls)});
  ASSERT_TRUE(server.ok());
  for (const auto& args : {nlohmann::json(nullptr), nlohmann::json::array(), nlohmann::json(5)}) {
    auto request = Call();
    request["params"]["arguments"] = args;
    EXPECT_EQ(Reply(*server, request)["error"]["code"], -32602);
  }
  for (const auto& name : {nlohmann::json(nullptr), nlohmann::json(5), nlohmann::json(""), nlohmann::json("missing")}) {
    auto request = Call();
    request["params"]["name"] = name;
    EXPECT_EQ(Reply(*server, request)["error"]["code"], -32602);
  }
  for (const std::string key : {"inputResponses", "requestState"}) {
    auto request = Call();
    request["params"][key] = nullptr;
    EXPECT_EQ(Reply(*server, request)["error"]["code"], -32602);
  }
  auto request = Call();
  request["params"].erase("name");
  EXPECT_EQ(Reply(*server, request)["error"]["code"], -32602);
  request = Call();
  request["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2025-11-25";
  EXPECT_EQ(Reply(*server, request)["error"]["code"], -32022);
  request = Call();
  request.erase("id");
  EXPECT_FALSE(server->Dispatch(json_dump(request)));
  EXPECT_EQ(calls, 0);
}

TEST(ServerToolsTest, SchemaErrorsAreActionableWithoutExecuting) {
  int calls = 0;
  auto server = Server::Create(Identity(), {Echo(&calls)});
  ASSERT_TRUE(server.ok());
  for (const auto& args :
       {nlohmann::json::object(), nlohmann::json{{"text", 42}}, nlohmann::json{{"text", "hello"}, {"extra", true}}}) {
    auto request = Call();
    request["params"]["arguments"] = args;
    auto reply = Reply(*server, request);
    EXPECT_EQ(reply["result"]["isError"], true);
    EXPECT_EQ(reply["result"]["content"][0]["type"], "text");
    EXPECT_FALSE(reply.contains("error"));
  }
  auto request = Call();
  request["params"].erase("arguments");
  EXPECT_EQ(Reply(*server, request)["result"]["isError"], true);
  EXPECT_EQ(calls, 0);
}

TEST(ServerToolsTest, MissingArgumentsDefaultToEmptyObject) {
  int calls = 0;
  auto tool = Echo(&calls);
  tool.definition.input_schema = {{"type", "object"}, {"additionalProperties", false}};
  tool.definition.output_schema = nlohmann::json::object();
  auto server = Server::Create(Identity(), {tool});
  ASSERT_TRUE(server.ok());
  auto request = Call();
  request["params"].erase("arguments");
  EXPECT_EQ(Reply(*server, request)["result"]["structuredContent"], nlohmann::json::object());
  EXPECT_EQ(calls, 1);
  auto catalog = Reply(*server, Request("tools/list"));
  EXPECT_FALSE(catalog["result"]["tools"][0].contains("outputSchema"));
}

TEST(ServerToolsTest, ToolFailuresDifferFromServerFailures) {
  int calls = 0;
  auto tool = Echo(&calls);
  tool.handler = [](const nlohmann::json&) -> absl::StatusOr<ToolCallResult> {
    ToolCallResult result;
    result.is_error = true;
    result.content.push_back({{"type", "text"}, {"text", "Try a different value"}});
    return result;
  };
  auto server = Server::Create(Identity(), {tool});
  ASSERT_TRUE(server.ok());
  EXPECT_EQ(Reply(*server, Call())["result"]["isError"], true);
  tool.handler = [](const nlohmann::json&) -> absl::StatusOr<ToolCallResult> {
    return absl::InternalError("private internal details");
  };
  server = Server::Create(Identity(), {tool});
  ASSERT_TRUE(server.ok());
  auto reply = Reply(*server, Call());
  EXPECT_EQ(reply["error"]["code"], -32603);
  EXPECT_EQ(reply["error"]["message"], "Tool handler failed");
}

TEST(ServerToolsTest, RejectsInvalidHandlerResults) {
  int calls = 0;
  const auto base = Echo(&calls);
  ToolCallResult valid;
  valid.content.push_back({{"type", "text"}, {"text", "hello"}});
  valid.structured_content = nlohmann::json{{"text", "hello"}};
  std::vector<ToolCallResult> invalid;
  auto result = valid;
  result.structured_content.reset();
  invalid.push_back(result);
  result = valid;
  result.structured_content = nlohmann::json{{"text", 42}};
  invalid.push_back(result);
  result = valid;
  result.content = {nlohmann::json{{"type", "text"}, {"text", false}}};
  invalid.push_back(result);
  result = valid;
  result.content = {nullptr};
  invalid.push_back(result);
  result = valid;
  result.meta = nullptr;
  invalid.push_back(result);
  result = valid;
  result.kind = ToolResultKind::kInputRequired;
  invalid.push_back(result);
  result = valid;
  result.request_state = "state";
  invalid.push_back(result);
  for (const auto& bad : invalid) {
    auto tool = base;
    tool.handler = [bad](const nlohmann::json&) -> absl::StatusOr<ToolCallResult> { return bad; };
    auto server = Server::Create(Identity(), {tool});
    ASSERT_TRUE(server.ok());
    EXPECT_EQ(Reply(*server, Call())["error"]["code"], -32603);
  }
}

TEST(ServerToolsTest, ValidatesContentBlockShapes) {
  int calls = 0;
  auto tool = Echo(&calls);
  tool.definition.output_schema = nlohmann::json::object();
  const std::vector<nlohmann::json> valid = {
      {{"type", "text"}, {"text", "hello"}},
      {{"type", "image"}, {"data", "aGVsbG8="}, {"mimeType", "image/png"}},
      {{"type", "audio"}, {"data", "aGVsbG8="}, {"mimeType", "audio/wav"}},
      {{"type", "resource_link"}, {"uri", "file:///example"}, {"name", "example"}},
      {{"type", "resource"}, {"resource", {{"uri", "file:///example"}, {"text", "hello"}}}},
      {{"type", "resource"}, {"resource", {{"uri", "file:///example"}, {"blob", "aGVsbG8="}}}},
      {{"type", "resource"}, {"resource", {{"uri", "file:///example"}, {"text", ""}}}},
      {{"type", "resource"}, {"resource", {{"uri", "file:///example"}, {"blob", ""}}}}};
  const std::vector<nlohmann::json> invalid = {
      {{"type", "unknown"}},
      {{"type", "image"}, {"data", "aGVsbG8="}},
      {{"type", "audio"}, {"data", false}, {"mimeType", "audio/wav"}},
      {{"type", "resource_link"}, {"uri", "file:///example"}},
      {{"type", "resource"}, {"resource", nullptr}},
      {{"type", "resource"}, {"resource", {{"uri", "file:///example"}, {"text", "hello"}, {"blob", "aA=="}}}},
      {{"type", "resource"}, {"resource", {{"uri", "file:///example"}}}},
      {{"type", "resource"}, {"resource", {{"uri", "file:///example"}, {"text", "hello"}, {"blob", 7}}}},
      {{"type", "resource"}, {"resource", {{"uri", "file:///example"}, {"blob", "aA=="}, {"text", 7}}}},
      {{"type", "resource"}, {"resource", {{"uri", "file:///example"}, {"text", "hello"}, {"blob", nullptr}}}},
      {{"type", "resource"}, {"resource", {{"uri", "file:///example"}, {"blob", "aA=="}, {"text", nullptr}}}},
      {{"type", "text"}, {"text", "hello"}, {"_meta", nullptr}},
      {{"type", "text"}, {"text", "hello"}, {"annotations", nullptr}}};
  for (const auto& block : valid) {
    tool.handler = [block](const nlohmann::json&) -> absl::StatusOr<ToolCallResult> {
      ToolCallResult result;
      result.content.push_back(block);
      return result;
    };
    auto server = Server::Create(Identity(), {tool});
    ASSERT_TRUE(server.ok());
    auto reply = Reply(*server, Call());
    EXPECT_TRUE(v2026_07_28::ParseToolCallResult(reply["result"]).ok());
    EXPECT_EQ(reply["result"]["content"][0], block);
  }
  for (const auto& block : invalid) {
    tool.handler = [block](const nlohmann::json&) -> absl::StatusOr<ToolCallResult> {
      ToolCallResult result;
      result.content.push_back(block);
      return result;
    };
    auto server = Server::Create(Identity(), {tool});
    ASSERT_TRUE(server.ok());
    EXPECT_EQ(Reply(*server, Call())["error"]["code"], -32603);
  }
}

TEST(ServerToolsTest, StructuredNullIsDifferentFromMissingOutput) {
  int calls = 0;
  auto tool = Echo(&calls);
  tool.definition.output_schema = {{"type", "null"}};
  tool.handler = [](const nlohmann::json&) -> absl::StatusOr<ToolCallResult> {
    ToolCallResult result;
    result.structured_content = nlohmann::json(nullptr);
    return result;
  };
  auto server = Server::Create(Identity(), {tool});
  ASSERT_TRUE(server.ok());
  const auto reply = Reply(*server, Call());
  ASSERT_TRUE(reply["result"].contains("structuredContent"));
  EXPECT_TRUE(reply["result"]["structuredContent"].is_null());
}

TEST(ServerToolsTest, StructuredOutputsMayBeNonObjects) {
  int calls = 0;
  auto tool = Echo(&calls);
  tool.definition.output_schema = {{"type", "array"}, {"items", {{"type", "string"}}}};
  tool.handler = [](const nlohmann::json&) -> absl::StatusOr<ToolCallResult> {
    ToolCallResult result;
    result.structured_content = nlohmann::json::array({"hello"});
    return result;
  };
  auto server = Server::Create(Identity(), {tool});
  ASSERT_TRUE(server.ok());
  EXPECT_EQ(Reply(*server, Call())["result"]["structuredContent"], nlohmann::json::array({"hello"}));
}

}  // namespace
}  // namespace slop::mcp::server
