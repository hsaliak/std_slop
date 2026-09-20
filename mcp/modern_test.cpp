#include "mcp/modern.h"

#include <string>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "core/json_utils.h"
#include "mcp/protocol.h"

namespace slop::mcp::v2026_07_28 {
namespace {

Request BaseRequest(std::string method = "server/discover") {
  Request request;
  request.id = std::string("request-1");
  request.method = std::move(method);
  request.context.client_info.name = "std_slop";
  request.context.client_info.version = "1.0";
  return request;
}

bool HasHeader(const EncodedRequest& request, absl::string_view expected) {
  for (const std::string& header : request.headers) {
    if (header == expected) return true;
  }
  return false;
}

TEST(ModernCodecTest, EncodesDiscoveryMetadataAndMirroredHeaders) {
  auto encoded = EncodeRequest(BaseRequest());
  ASSERT_TRUE(encoded.ok()) << encoded.status();
  EXPECT_EQ(json_get_or(encoded->body, "jsonrpc", std::string{}), "2.0");
  EXPECT_EQ(json_get_or(encoded->body, "method", std::string{}), "server/discover");
  const auto& meta = encoded->body["params"]["_meta"];
  EXPECT_EQ(json_get_or(meta, kProtocolVersionMetadata, std::string{}), kModernProtocolVersion);
  EXPECT_TRUE(json_at(meta, kClientCapabilitiesMetadata)->is_object());
  EXPECT_EQ(json_get_or(*json_at(meta, kClientInfoMetadata), "name", std::string{}), "std_slop");
  EXPECT_TRUE(HasHeader(*encoded, "MCP-Protocol-Version: 2026-07-28"));
  EXPECT_TRUE(HasHeader(*encoded, "Mcp-Method: server/discover"));
}

TEST(ModernCodecTest, EncodesToolNameAndAnnotatedArguments) {
  Request request = BaseRequest("tools/call");
  request.params = {{"name", " snow "},
                    {"arguments", {{"tenant", " acme "}, {"count", 9}, {"nested", {{"enabled", true}}}}}};
  request.tool_schema = {
      {"type", "object"},
      {"properties",
       {{"tenant", {{"type", "string"}, {"x-mcp-header", "Tenant"}}},
        {"count", {{"type", "integer"}, {"x-mcp-header", "Count"}}},
        {"nested",
         {{"type", "object"}, {"properties", {{"enabled", {{"type", "boolean"}, {"x-mcp-header", "Enabled"}}}}}}}}},
  };

  auto encoded = EncodeRequest(request);
  ASSERT_TRUE(encoded.ok()) << encoded.status();
  EXPECT_TRUE(HasHeader(*encoded, "Mcp-Name: =?base64?IHNub3cgAw==?="));
  EXPECT_TRUE(HasHeader(*encoded, "Mcp-Param-Tenant: =?base64?IGFjbWUg?="));
  EXPECT_TRUE(HasHeader(*encoded, "Mcp-Param-Count: 9"));
  EXPECT_TRUE(HasHeader(*encoded, "Mcp-Param-Enabled: true"));
}

TEST(ModernCodecTest, RejectsInvalidAnnotationsAndArgumentValues) {
  Request request = BaseRequest("tools/call");
  request.params = {{"name", "tool"}, {"arguments", {{"count", 9007199254740992ULL}}}};
  request.tool_schema = {
      {"type", "object"},
      {"properties", {{"count", {{"type", "integer"}, {"x-mcp-header", "Count"}}}}},
  };
  EXPECT_EQ(EncodeRequest(request).status().code(), absl::StatusCode::kInvalidArgument);

  request.tool_schema = {
      {"type", "object"},
      {"properties",
       {{"one", {{"type", "string"}, {"x-mcp-header", "X-ID"}}},
        {"two", {{"type", "string"}, {"x-mcp-header", "x-id"}}}}},
  };
  EXPECT_EQ(EncodeRequest(request).status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(ModernCodecTest, ParsesDiscoveryAndResultVariants) {
  const nlohmann::json discovery_json = {
      {"supportedVersions", {"2026-07-28"}},
      {"capabilities", {{"tools", nlohmann::json::object()}}},
      {"instructions", "be concise"},
      {"_meta", {{kServerInfoMetadata, {{"name", "server"}, {"version", "2"}}}}},
  };
  auto discovery = ParseDiscovery(discovery_json);
  ASSERT_TRUE(discovery.ok()) << discovery.status();
  ASSERT_TRUE(discovery->server_info.has_value());
  EXPECT_EQ(discovery->server_info->name, "server");

  auto implicit = ParseResult({{"value", 1}});
  ASSERT_TRUE(implicit.ok());
  EXPECT_EQ(implicit->type, ResultType::kComplete);
  auto input = ParseResult({{"resultType", "input_required"}, {"requestState", {1, 2}}});
  ASSERT_TRUE(input.ok());
  EXPECT_EQ(input->type, ResultType::kInputRequired);
  EXPECT_TRUE(input->request_state.has_value());
  EXPECT_EQ(ParseResult({{"resultType", "future"}}).status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(ModernCodecTest, ExcludesOnlyInvalidTools) {
  const nlohmann::json result = {
      {"resultType", "complete"},
      {"tools",
       {{{"name", "good"}, {"inputSchema", {{"type", "object"}}}, {"_meta", {{"owner", "team"}}}},
        {{"name", "bad"},
         {"inputSchema",
          {{"type", "object"}, {"properties", {{"value", {{"type", "array"}, {"x-mcp-header", "Invalid"}}}}}}}}}},
  };
  auto tools = ParseToolsList(result);
  ASSERT_TRUE(tools.ok()) << tools.status();
  ASSERT_EQ(tools->size(), 1);
  EXPECT_EQ((*tools)[0].name, "good");
  EXPECT_EQ(json_get_or((*tools)[0].meta, "owner", std::string{}), "team");
}

TEST(ModernCodecTest, PreservesStructuredContentPresenceAndMetadata) {
  const nlohmann::json result = {
      {"content", nlohmann::json::array({{{"type", "text"}, {"text", "ok"}}})},
      {"structuredContent", nullptr},
      {"_meta", {{"trace", "opaque"}}},
  };
  auto parsed = ParseToolCallResult(result);
  ASSERT_TRUE(parsed.ok()) << parsed.status();
  ASSERT_TRUE(parsed->structured_content.has_value());
  EXPECT_TRUE(parsed->structured_content->is_null());
  EXPECT_EQ(json_get_or(parsed->meta, "trace", std::string{}), "opaque");
}

}  // namespace
}  // namespace slop::mcp::v2026_07_28
