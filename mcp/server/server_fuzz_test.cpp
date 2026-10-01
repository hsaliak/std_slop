#include <string>

#include "gtest/gtest.h"

#include "core/json_utils.h"
#include "fuzztest/fuzztest.h"
#include "mcp/json_rpc.h"
#include "mcp/server/server.h"

namespace slop::mcp::server {
namespace {

void DispatchNeverCrashes(const std::string& raw) {
  ImplementationInfo identity;
  identity.name = "fuzz";
  identity.version = "1";
  auto server = Server::Create(identity);
  ASSERT_TRUE(server.ok());
  const auto reply = server->Dispatch(raw);
  EXPECT_EQ(reply, server->Dispatch(raw));
  if (!reply) return;
  EXPECT_TRUE(ParseJsonRpcResponse(*reply).ok());
  if (json_at(*reply, "result") == nullptr) return;
  const auto input = json_parse(raw);
  ASSERT_TRUE(input);
  EXPECT_EQ(json_get_or(*input, "method", std::string{}), "server/discover");
  EXPECT_EQ(json_get_or(*input, "jsonrpc", std::string{}), "2.0");
  EXPECT_NE(json_at(*input, "id"), nullptr);
}
FUZZ_TEST(ServerFuzzTest, DispatchNeverCrashes);

void InvalidMetadataNeverSucceeds(const std::string& raw_meta) {
  const auto meta = json_parse(raw_meta);
  if (!meta) return;
  ImplementationInfo identity;
  identity.name = "fuzz";
  identity.version = "1";
  auto server = Server::Create(identity);
  ASSERT_TRUE(server.ok());
  nlohmann::json request = {
      {"jsonrpc", "2.0"}, {"id", 1}, {"method", "server/discover"}, {"params", {{"_meta", *meta}}}};
  const auto reply = server->Dispatch(json_dump(request));
  ASSERT_TRUE(reply);
  if (!meta->is_object() ||
      json_get_or(*meta, "io.modelcontextprotocol/protocolVersion", std::string{}) != "2026-07-28") {
    EXPECT_EQ(json_at(*reply, "result"), nullptr);
  }
}
FUZZ_TEST(ServerFuzzTest, InvalidMetadataNeverSucceeds);

TEST(ServerFuzzTest, RegressionSeeds) {
  for (const std::string raw : {"", "null", "[]", "{", R"({"jsonrpc":"2.0","id":true,"method":"server/discover"})"}) {
    DispatchNeverCrashes(raw);
  }
  for (const std::string raw : {"null", "[]", "{}", R"({"io.modelcontextprotocol/protocolVersion":"2025-11-25"})"}) {
    InvalidMetadataNeverSucceeds(raw);
  }
}

}  // namespace
}  // namespace slop::mcp::server
