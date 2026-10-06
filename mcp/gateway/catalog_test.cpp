#include "mcp/gateway/catalog.h"

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "gtest/gtest.h"

#include "mcp/protocol.h"

namespace slop::mcp::gateway {
namespace {

class FakeClient final : public Client {
 public:
  ProtocolRevision revision() const override { return ProtocolRevision::k2026_07_28; }
  absl::StatusOr<std::vector<Tool>> ListTools() override { return tools; }
  absl::StatusOr<ToolCallResult> CallTool(const std::string& name, const nlohmann::json& arguments,
                                          absl::Duration) override {
    called_name = name;
    called_arguments = arguments;
    return result;
  }
  absl::StatusOr<ToolCallResult> ContinueToolCall(const std::string&, const nlohmann::json&, const nlohmann::json&,
                                                  absl::Duration) override {
    return absl::UnimplementedError("continuation not used");
  }

  std::vector<Tool> tools;
  ToolCallResult result;
  std::string called_name;
  nlohmann::json called_arguments;
};

Tool EchoTool() {
  Tool tool;
  tool.name = "echo";
  tool.description = "Return the text";
  tool.input_schema = {{"type", "object"},
                       {"properties", {{"text", {{"type", "string"}}}}},
                       {"required", {"text"}},
                       {"additionalProperties", false}};
  return tool;
}

std::unique_ptr<FakeClient> MakeFakeClient() {
  auto client = std::make_unique<FakeClient>();
  Tool other = EchoTool();
  other.name = "other";
  client->tools = {EchoTool(), other};
  client->result.structured_content = {{"text", "ok"}};
  return client;
}

absl::StatusOr<Catalog> MakeCatalog(std::vector<std::string> grants, std::unique_ptr<FakeClient> client) {
  std::vector<DownstreamClient> servers;
  servers.push_back({"echo", std::move(grants), std::move(client)});
  return Catalog::Create(std::move(servers));
}

TEST(CatalogTest, AppliesAllowListToDiscoveredTools) {
  auto client = MakeFakeClient();
  FakeClient* raw = client.get();
  auto catalog = MakeCatalog({"echo"}, std::move(client));
  ASSERT_TRUE(catalog.ok()) << catalog.status();
  EXPECT_EQ(catalog->size(), 1);
  EXPECT_FALSE(catalog->Call("echo", "other", {{"text", "hidden"}},
                             std::chrono::steady_clock::now() + std::chrono::seconds(1))
                   .ok());
  auto result =
      catalog->Call("echo", "echo", {{"text", "hello"}}, std::chrono::steady_clock::now() + std::chrono::seconds(1));
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(raw->called_name, "echo");
  EXPECT_EQ(raw->called_arguments, nlohmann::json({{"text", "hello"}}));
}

TEST(CatalogTest, RejectsToolNamesThatExceedRuntimeLimits) {
  auto client = MakeFakeClient();
  client->tools[0].name = std::string(257, 'x');
  EXPECT_FALSE(MakeCatalog({"echo"}, std::move(client)).ok());
}

TEST(CatalogTest, EmptyGrantExposesAllDiscoveredTools) {
  auto catalog = MakeCatalog({}, MakeFakeClient());
  ASSERT_TRUE(catalog.ok()) << catalog.status();
  EXPECT_EQ(catalog->size(), 2);
  auto result =
      catalog->Call("echo", "other", {{"text", "x"}}, std::chrono::steady_clock::now() + std::chrono::seconds(1));
  ASSERT_TRUE(result.ok()) << result.status();
}

TEST(CatalogTest, RejectsUndeclaredOrInvalidCallsBeforeClientExecution) {
  auto client = MakeFakeClient();
  FakeClient* raw = client.get();
  auto catalog = MakeCatalog({"echo"}, std::move(client));
  ASSERT_TRUE(catalog.ok()) << catalog.status();
  EXPECT_FALSE(
      catalog->Call("wrong", "echo", {{"text", "x"}}, std::chrono::steady_clock::now() + std::chrono::seconds(1)).ok());
  EXPECT_FALSE(
      catalog->Call("echo", "missing", {{"text", "x"}}, std::chrono::steady_clock::now() + std::chrono::seconds(1))
          .ok());
  EXPECT_FALSE(
      catalog->Call("echo", "echo", {{"text", 1}}, std::chrono::steady_clock::now() + std::chrono::seconds(1)).ok());
  EXPECT_TRUE(raw->called_name.empty());
}

TEST(CatalogTest, BuildsSummaryAndFocusedHelpFromAuthorizedRoutes) {
  auto catalog = MakeCatalog({"echo"}, MakeFakeClient());
  ASSERT_TRUE(catalog.ok()) << catalog.status();
  const nlohmann::json public_catalog = catalog->PublicCatalog();
  ASSERT_EQ(public_catalog.size(), 1);
  EXPECT_EQ(public_catalog[0]["alias"], "echo");
  ASSERT_EQ(public_catalog[0]["tools"].size(), 1);
  EXPECT_EQ(public_catalog[0]["tools"][0]["inputSchema"]["required"], nlohmann::json({"text"}));
  auto focused = catalog->Help("echo", "echo");
  ASSERT_TRUE(focused.ok()) << focused.status();
  EXPECT_TRUE((*focused)["tools"][0].contains("inputSchema"));
  EXPECT_FALSE(catalog->Help("echo", "missing").ok());
}

}  // namespace
}  // namespace slop::mcp::gateway
