#include "mcp/decision_api/server.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "core/json_utils.h"
#include "mcp/server/server.h"

namespace slop::mcp::decision_api {
namespace {

class FakeHttpClient final : public slop::HttpClient {
 public:
  absl::StatusOr<slop::HttpResponse> PostOnceStreamWithResponse(const std::string&, const std::string&,
                                                                const std::vector<std::string>&, absl::Duration,
                                                                std::size_t, ChunkCallback) override {
    ++post_count;
    slop::HttpResponse response;
    response.status_code = 200;
    response.body =
        R"({"model":"~typesafe/jev-latest","answers":{"urgent":{"type":"noul","noul":0.8}},"usage":{"input_tokens":1,"output_tokens":1}})";
    return response;
  }

  absl::StatusOr<slop::HttpResponse> GetOnceWithResponse(const std::string&, const std::vector<std::string>&,
                                                         absl::Duration, std::size_t) override {
    ++get_count;
    return slop::HttpResponse{200, R"({"data":[{"id":"~typesafe/jev-latest"}],"total_count":1})", {}};
  }

  int post_count = 0;
  int get_count = 0;
};

std::optional<nlohmann::json> DispatchTool(const mcp::server::Server& server, const std::string& name,
                                           const nlohmann::json& arguments) {
  const nlohmann::json request = {{"jsonrpc", "2.0"},
                                  {"id", 1},
                                  {"method", "tools/call"},
                                  {"params",
                                   {{"name", name},
                                    {"arguments", arguments},
                                    {"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}}}}}};
  return server.Dispatch(json_dump(request));
}

nlohmann::json ValidArguments() {
  return {{"state", "Customer requests a refund."},
          {"questions", {{"urgent", {{"type", "noul"}, {"instructions", "Is this urgent?"}}}}}};
}

TEST(DecisionApiServerTest, AdvertisesCorrectToolsAndAgentHelpWithoutNetwork) {
  Config config;
  config.model = "~typesafe/jev-latest";
  config.endpoint = "https://example.test/decisions?key=not-for-help";
  FakeHttpClient http;
  auto client = std::make_shared<DecisionApiClient>(config, &http);
  auto server = CreateServer(client, config);
  ASSERT_TRUE(server.ok()) << server.status();

  const nlohmann::json discovery_request = {
      {"jsonrpc", "2.0"},
      {"id", "discover"},
      {"method", "server/discover"},
      {"params", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}}}}}};
  const auto discovery = server->Dispatch(json_dump(discovery_request));
  ASSERT_TRUE(discovery.has_value());
  const auto* discovery_result = json_at(*discovery, "result");
  ASSERT_NE(discovery_result, nullptr);
  const auto* discovery_meta = json_at(*discovery_result, "_meta");
  ASSERT_NE(discovery_meta, nullptr);
  const auto* server_info = json_at(*discovery_meta, "io.modelcontextprotocol/serverInfo");
  ASSERT_NE(server_info, nullptr);
  EXPECT_EQ(json_get<std::string>(*server_info, "name"), "decision-api-mcp");
  EXPECT_EQ(json_get<std::string>(*server_info, "title"), "Decision API MCP");

  const nlohmann::json list_request = {
      {"jsonrpc", "2.0"},
      {"id", 1},
      {"method", "tools/list"},
      {"params", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}}}}}};
  const auto listed = server->Dispatch(json_dump(list_request));
  ASSERT_TRUE(listed.has_value());
  const std::string listing = json_dump(*listed);
  EXPECT_NE(listing.find("decision_help"), std::string::npos);
  EXPECT_NE(listing.find("openWorldHint"), std::string::npos);
  EXPECT_NE(listing.find("SystemOne"), std::string::npos);
  EXPECT_NE(listing.find("TypeSafe Jev"), std::string::npos);
  EXPECT_NE(listing.find("OpenRouter is the default backend"), std::string::npos);
  EXPECT_EQ(http.post_count, 0);
  EXPECT_EQ(http.get_count, 0);

  const auto help = DispatchTool(*server, "decision_help", {{"topic", "noul"}});
  ASSERT_TRUE(help.has_value());
  EXPECT_NE(json_dump(*help).find("P(yes)"), std::string::npos);
  const auto overview = DispatchTool(*server, "decision_help", {{"topic", "overview"}});
  ASSERT_TRUE(overview.has_value());
  EXPECT_EQ(json_dump(*overview).find("not-for-help"), std::string::npos);
  EXPECT_NE(json_dump(*overview).find("SystemOne"), std::string::npos);
  EXPECT_NE(json_dump(*overview).find("TypeSafe"), std::string::npos);
  EXPECT_NE(json_dump(*overview).find("OpenRouter is the default backend"), std::string::npos);

  const auto* list_result = json_at(*listed, "result");
  ASSERT_NE(list_result, nullptr);
  const auto* tool_definitions = json_at(*list_result, "tools");
  ASSERT_NE(tool_definitions, nullptr);
  const nlohmann::json* decide_schema = nullptr;
  for (const auto& definition : *tool_definitions) {
    if (json_get<std::string>(definition, "name") == "decide") decide_schema = json_at(definition, "inputSchema");
  }
  ASSERT_NE(decide_schema, nullptr);
  const auto* help_result = json_at(*overview, "result");
  ASSERT_NE(help_result, nullptr);
  const auto* help_content = json_at(*help_result, "structuredContent");
  ASSERT_NE(help_content, nullptr);
  const auto* help_schemas = json_at(*help_content, "schemas");
  ASSERT_NE(help_schemas, nullptr);
  const auto* help_decide_schema = json_at(*help_schemas, "decide");
  ASSERT_NE(help_decide_schema, nullptr);
  EXPECT_EQ(*help_decide_schema, *decide_schema);

  const auto* examples = json_at(*help_content, "examples");
  ASSERT_NE(examples, nullptr);
  const auto* request_example = json_at(*examples, "request");
  const auto* response_example = json_at(*examples, "response");
  ASSERT_NE(request_example, nullptr);
  ASSERT_NE(response_example, nullptr);
  auto example_request = DecisionApiClient::BuildRequest(*request_example, config);
  ASSERT_TRUE(example_request.ok()) << example_request.status();
  EXPECT_TRUE(DecisionApiClient::ValidateResponse(*example_request, *response_example).ok());
  EXPECT_EQ(http.post_count, 0);
  EXPECT_EQ(http.get_count, 0);
}

TEST(DecisionApiServerTest, DispatchesEvaluationAndModelDiscovery) {
  Config config;
  config.api_key = "test-secret";
  config.model = "~typesafe/jev-latest";
  FakeHttpClient http;
  auto client = std::make_shared<DecisionApiClient>(config, &http);
  auto server = CreateServer(client, config);
  ASSERT_TRUE(server.ok()) << server.status();

  const auto decision = DispatchTool(*server, "decide", ValidArguments());
  ASSERT_TRUE(decision.has_value());
  EXPECT_EQ(http.post_count, 1);
  EXPECT_NE(json_dump(*decision).find("~typesafe/jev-latest"), std::string::npos);
  EXPECT_EQ(json_dump(*decision).find("test-secret"), std::string::npos);

  const auto models = DispatchTool(*server, "decision_models", nlohmann::json::object());
  ASSERT_TRUE(models.has_value());
  EXPECT_EQ(http.get_count, 1);
  EXPECT_NE(json_dump(*models).find("~typesafe/jev-latest"), std::string::npos);
}

TEST(DecisionApiServerTest, InvalidToolArgumentsDoNotReachProvider) {
  Config config;
  FakeHttpClient http;
  auto client = std::make_shared<DecisionApiClient>(config, &http);
  auto server = CreateServer(client, config);
  ASSERT_TRUE(server.ok()) << server.status();
  auto invalid = ValidArguments();
  invalid["extra"] = true;
  const auto response = DispatchTool(*server, "decide", invalid);
  ASSERT_TRUE(response.has_value());
  EXPECT_NE(json_dump(*response).find("isError"), std::string::npos);
  EXPECT_EQ(http.post_count, 0);
}

}  // namespace
}  // namespace slop::mcp::decision_api
