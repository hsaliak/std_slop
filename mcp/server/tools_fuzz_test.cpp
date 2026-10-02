#include <string>
#include <utility>

#include "gtest/gtest.h"

#include "core/json_utils.h"
#include "fuzztest/fuzztest.h"
#include "mcp/json_rpc.h"
#include "mcp/server/server.h"

namespace slop::mcp::server {
namespace {

ImplementationInfo Identity() {
  ImplementationInfo info;
  info.name = "fuzz";
  info.version = "1";
  return info;
}

ToolRegistration Echo(int* calls) {
  ToolRegistration tool;
  tool.definition.name = "echo";
  tool.definition.input_schema = {{"type", "object"},
                                  {"required", {"text"}},
                                  {"properties", {{"text", {{"type", "string"}}}}},
                                  {"additionalProperties", false}};
  tool.handler = [calls](const nlohmann::json& arguments) -> absl::StatusOr<ToolCallResult> {
    ++*calls;
    EXPECT_TRUE(arguments.is_object());
    EXPECT_EQ(arguments.size(), 1);
    EXPECT_TRUE(json_get<std::string>(arguments, "text"));
    ToolCallResult result;
    result.content.push_back({{"type", "text"}, {"text", json_get_or(arguments, "text", std::string{})}});
    return result;
  };
  return tool;
}

nlohmann::json Call() {
  return {{"jsonrpc", "2.0"},
          {"id", 1},
          {"method", "tools/call"},
          {"params",
           {{"name", "echo"},
            {"arguments", {{"text", "seed"}}},
            {"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}}}}}};
}

void InvalidArgumentsNeverExecute(const std::string& raw_arguments) {
  const auto arguments = json_parse(raw_arguments);
  if (!arguments) return;
  int calls = 0;
  auto server = Server::Create(Identity(), {Echo(&calls)});
  ASSERT_TRUE(server.ok());
  auto request = Call();
  request["params"]["arguments"] = *arguments;
  const auto reply = server->Dispatch(json_dump(request));
  ASSERT_TRUE(reply);
  EXPECT_TRUE(ParseJsonRpcResponse(*reply).ok());
  const bool valid =
      arguments->is_object() && arguments->size() == 1 && json_get<std::string>(*arguments, "text").has_value();
  EXPECT_EQ(calls, valid ? 1 : 0);
  if (valid) {
    EXPECT_EQ((*reply)["result"]["isError"], false);
  } else if (arguments->is_object()) {
    EXPECT_EQ((*reply)["result"]["isError"], true);
  } else {
    EXPECT_EQ((*reply)["error"]["code"], -32602);
  }
}
FUZZ_TEST(ServerToolsFuzzTest, InvalidArgumentsNeverExecute);

void RawCallsCannotBypassValidation(const std::string& raw) {
  int calls = 0;
  auto server = Server::Create(Identity(), {Echo(&calls)});
  ASSERT_TRUE(server.ok());
  const auto reply = server->Dispatch(raw);
  EXPECT_LE(calls, 1);
  if (reply) EXPECT_TRUE(ParseJsonRpcResponse(*reply).ok());
  if (calls == 0) return;
  ASSERT_TRUE(reply);
  const auto input = json_parse(raw);
  ASSERT_TRUE(input);
  EXPECT_EQ(json_get_or(*input, "method", std::string{}), "tools/call");
  EXPECT_EQ(json_get_or(*input, "jsonrpc", std::string{}), "2.0");
  const auto* id = json_at(*input, "id");
  ASSERT_NE(id, nullptr);
  EXPECT_FALSE(id->is_null());
  const auto* params = json_at(*input, "params");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(json_get_or(*params, "name", std::string{}), "echo");
  EXPECT_EQ(json_at(*params, "requestState"), nullptr);
  EXPECT_EQ(json_at(*params, "inputResponses"), nullptr);
  const auto* meta = json_at(*params, "_meta");
  ASSERT_NE(meta, nullptr);
  EXPECT_EQ(json_get_or(*meta, "io.modelcontextprotocol/protocolVersion", std::string{}), "2026-07-28");
}
FUZZ_TEST(ServerToolsFuzzTest, RawCallsCannotBypassValidation);

void UnsupportedMetadataCannotExecute(const std::string& raw_meta) {
  const auto meta = json_parse(raw_meta);
  if (!meta) return;
  int calls = 0;
  auto server = Server::Create(Identity(), {Echo(&calls)});
  ASSERT_TRUE(server.ok());
  auto request = Call();
  request["params"]["_meta"] = *meta;
  const auto reply = server->Dispatch(json_dump(request));
  ASSERT_TRUE(reply);
  EXPECT_TRUE(ParseJsonRpcResponse(*reply).ok());
  if (!meta->is_object() ||
      json_get_or(*meta, "io.modelcontextprotocol/protocolVersion", std::string{}) != "2026-07-28") {
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(json_at(*reply, "result"), nullptr);
  }
}
FUZZ_TEST(ServerToolsFuzzTest, UnsupportedMetadataCannotExecute);

void NegatedRecursiveArgumentsNeverExecute(unsigned int depth) {
  int calls = 0;
  auto tool = Echo(&calls);
  tool.definition.input_schema = {
      {"type", "object"},
      {"not", {{"$ref", "#/$defs/node"}}},
      {"$defs", {{"node", {{"type", "object"}, {"properties", {{"next", {{"$ref", "#/$defs/node"}}}}}}}}}};
  auto server = Server::Create(Identity(), {tool});
  ASSERT_TRUE(server.ok());
  nlohmann::json arguments = nlohmann::json::object();
  for (unsigned int i = 0; i < depth % 81; ++i) arguments = {{"next", std::move(arguments)}};
  auto request = Call();
  request["params"]["arguments"] = arguments;
  const auto reply = server->Dispatch(json_dump(request));
  ASSERT_TRUE(reply);
  EXPECT_EQ(calls, 0);
  EXPECT_EQ((*reply)["result"]["isError"], true);
}
FUZZ_TEST(ServerToolsFuzzTest, NegatedRecursiveArgumentsNeverExecute);

void HandlerContentNeverCrashes(const std::string& raw_content) {
  const auto content = json_parse(raw_content);
  if (!content) return;
  int calls = 0;
  auto tool = Echo(&calls);
  tool.handler = [content](const nlohmann::json&) -> absl::StatusOr<ToolCallResult> {
    ToolCallResult result;
    result.content = {*content};
    return result;
  };
  auto server = Server::Create(Identity(), {tool});
  ASSERT_TRUE(server.ok());
  const auto reply = server->Dispatch(json_dump(Call()));
  ASSERT_TRUE(reply);
  EXPECT_TRUE(ParseJsonRpcResponse(*reply).ok());
  if (!content->is_object() || !json_get<std::string>(*content, "type")) {
    EXPECT_EQ((*reply)["error"]["code"], -32603);
  }
}
FUZZ_TEST(ServerToolsFuzzTest, HandlerContentNeverCrashes);

void ResourceVariantsAreValidatedByPresence(const std::string& raw_resource) {
  const auto resource = json_parse(raw_resource);
  if (!resource) return;
  int calls = 0;
  auto tool = Echo(&calls);
  tool.handler = [resource](const nlohmann::json&) -> absl::StatusOr<ToolCallResult> {
    ToolCallResult result;
    result.content.push_back({{"type", "resource"}, {"resource", *resource}});
    return result;
  };
  auto server = Server::Create(Identity(), {tool});
  ASSERT_TRUE(server.ok());
  const auto reply = server->Dispatch(json_dump(Call()));
  ASSERT_TRUE(reply);
  EXPECT_TRUE(ParseJsonRpcResponse(*reply).ok());
  const auto* text = json_at(*resource, "text");
  const auto* blob = json_at(*resource, "blob");
  const bool valid = resource->is_object() && json_get<std::string>(*resource, "uri") &&
                     ((text != nullptr && text->is_string() && blob == nullptr) ||
                      (blob != nullptr && blob->is_string() && text == nullptr));
  if (valid) {
    const auto* result = json_at(*reply, "result");
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(json_get_or(*result, "isError", true), false);
  } else {
    const auto* error = json_at(*reply, "error");
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(json_get_or(*error, "code", 0), -32603);
  }
}
FUZZ_TEST(ServerToolsFuzzTest, ResourceVariantsAreValidatedByPresence);

TEST(ServerToolsFuzzTest, RegressionSeeds) {
  for (unsigned int depth : {0u, 4u, 40u, 80u}) NegatedRecursiveArgumentsNeverExecute(depth);
  for (const std::string resource :
       {"null", "{}", R"({"uri":"file:///example","text":""})", R"({"uri":"file:///example","blob":""})",
        R"({"uri":"file:///example","text":"ok","blob":7})", R"({"uri":"file:///example","blob":"aA==","text":7})",
        R"({"uri":"file:///example","text":"ok","blob":null})",
        R"({"uri":"file:///example","blob":"aA==","text":null})"}) {
    ResourceVariantsAreValidatedByPresence(resource);
  }
  for (const std::string args :
       {"null", "[]", "{}", R"({"text":1})", R"({"text":"ok"})", R"({"text":"ok","extra":true})"}) {
    InvalidArgumentsNeverExecute(args);
  }
  auto request = Call();
  RawCallsCannotBypassValidation(json_dump(request));
  request["id"] = true;
  RawCallsCannotBypassValidation(json_dump(request));
  request.erase("id");
  RawCallsCannotBypassValidation(json_dump(request));
  for (const std::string meta : {"null", "[]", "{}", R"({"io.modelcontextprotocol/protocolVersion":"2025-11-25"})"}) {
    UnsupportedMetadataCannotExecute(meta);
  }
  for (const std::string content : {"null", "{}", R"({"type":"text","text":7})", R"({"type":"text","text":"ok"})"}) {
    HandlerContentNeverCrashes(content);
  }
}

}  // namespace
}  // namespace slop::mcp::server
