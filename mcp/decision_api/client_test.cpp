#include "mcp/decision_api/client.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/time/time.h"
#include "gtest/gtest.h"

#include "core/json_utils.h"

namespace slop::mcp::decision_api {
namespace {

class FakeHttpClient final : public slop::HttpClient {
 public:
  absl::StatusOr<slop::HttpResponse> PostOnceStreamWithResponse(const std::string& url, const std::string& body,
                                                                const std::vector<std::string>& headers, absl::Duration,
                                                                std::size_t, ChunkCallback) override {
    ++post_count;
    last_url = url;
    last_body = body;
    last_headers = headers;
    return post_response;
  }

  absl::StatusOr<slop::HttpResponse> GetOnceWithResponse(const std::string& url,
                                                         const std::vector<std::string>& headers, absl::Duration,
                                                         std::size_t) override {
    ++get_count;
    last_url = url;
    last_headers = headers;
    return slop::HttpResponse{get_status, get_body, {}};
  }

  int post_count = 0;
  int get_count = 0;
  long get_status = 200;
  std::string last_url;
  std::string last_body;
  std::vector<std::string> last_headers;
  slop::HttpResponse post_response;
  std::string get_body;
};

nlohmann::json RequestArguments() {
  return {{"state", {{"ticket", "refund requested"}}},
          {"questions", {{"refund", {{"type", "noul"}, {"instructions", "Is a refund requested?"}}}}}};
}

nlohmann::json ValidResponse() {
  return {{"model", "~typesafe/jev-latest"},
          {"usage", {{"input_tokens", 5}, {"output_tokens", 2}}},
          {"answers", {{"refund", {{"type", "noul"}, {"noul", 0.9}}}}}};
}

TEST(DecisionApiClientTest, BuildsRequestAndForwardsModelAliasUnchanged) {
  Config config;
  config.model = "configured-model";
  nlohmann::json args = RequestArguments();
  args["model"] = "~typesafe/jev-latest";
  args["openrouterOptions"] = {
      {"session_id", "session-1"}, {"trace", {{"generation_name", "billing-check"}}}, {"user", "agent-1"}};
  auto request = DecisionApiClient::BuildRequest(args, config);
  ASSERT_TRUE(request.ok()) << request.status();
  EXPECT_EQ(json_get<std::string>(*request, "model"), "~typesafe/jev-latest");
  EXPECT_EQ(json_get<std::string>(*request, "session_id"), "session-1");
  EXPECT_EQ(json_get<std::string>(*request, "user"), "agent-1");
  EXPECT_NE(json_at(*request, "trace"), nullptr);
}

TEST(DecisionApiClientTest, RejectsInvalidQuestionAndUnknownFields) {
  Config config;
  auto args = RequestArguments();
  args["questions"]["refund"]["type"] = "unknown";
  EXPECT_FALSE(DecisionApiClient::BuildRequest(args, config).ok());
  args = RequestArguments();
  args["openrouterOptions"] = {{"unexpected", "value"}};
  EXPECT_FALSE(DecisionApiClient::BuildRequest(args, config).ok());
  args = RequestArguments();
  args["questions"]["refund"]["criteria"] = {{"true", "yes"}, {"false", "no"}, {"other", "bad"}};
  EXPECT_FALSE(DecisionApiClient::BuildRequest(args, config).ok());
  args = RequestArguments();
  args["openrouterOptions"] = {{"trace", {{"unknown", "value"}}}};
  EXPECT_FALSE(DecisionApiClient::BuildRequest(args, config).ok());
}

TEST(DecisionApiClientTest, ValidatesCorrelatedAnswerAndRequiredResponseFields) {
  auto args = RequestArguments();
  Config config;
  auto request = DecisionApiClient::BuildRequest(args, config);
  ASSERT_TRUE(request.ok()) << request.status();
  EXPECT_TRUE(DecisionApiClient::ValidateResponse(*request, ValidResponse()).ok());
  auto response = ValidResponse();
  response["answers"]["refund"]["noul"] = 1.1;
  EXPECT_FALSE(DecisionApiClient::ValidateResponse(*request, response).ok());
  response = ValidResponse();
  response["answers"].erase("refund");
  EXPECT_FALSE(DecisionApiClient::ValidateResponse(*request, response).ok());
  response = ValidResponse();
  response.erase("usage");
  EXPECT_FALSE(DecisionApiClient::ValidateResponse(*request, response).ok());
  response = ValidResponse();
  response["usage"].erase("input_tokens");
  EXPECT_FALSE(DecisionApiClient::ValidateResponse(*request, response).ok());
}

TEST(DecisionApiClientTest, ValidatesChoiceAndScoreAndOptionalFields) {
  Config config;
  nlohmann::json args = {
      {"state", "ticket"},
      {"questions",
       {{"team",
         {{"type", "choice"},
          {"instructions", "Choose a team"},
          {"criteria", {{"billing", "Money"}, {"support", "Help"}}}}},
        {"urgency", {{"type", "score"}, {"instructions", "Rate urgency"}, {"criteria", {"low", "high"}}}}}}};
  auto request = DecisionApiClient::BuildRequest(args, config);
  ASSERT_TRUE(request.ok()) << request.status();
  nlohmann::json response = {
      {"model", "versioned-model"},
      {"usage", {{"input_tokens", 3}, {"output_tokens", 1}}},
      {"answers",
       {{"team", {{"type", "choice"}, {"choice", "billing"}, {"probabilities", {{"billing", 0.8}, {"support", 0.2}}}}},
        {"urgency",
         {{"type", "score"},
          {"score", 0.75},
          {"legend", {{"0", "low"}, {"1", "high"}}},
          {"probabilities", {{"0", 0.25}, {"1", 0.75}}}}}}}};
  const absl::Status valid_status = DecisionApiClient::ValidateResponse(*request, response);
  EXPECT_TRUE(valid_status.ok()) << valid_status;
  response["answers"]["team"]["choice"] = "unknown";
  EXPECT_FALSE(DecisionApiClient::ValidateResponse(*request, response).ok());
  response["answers"]["team"]["choice"] = "billing";
  response["answers"]["urgency"]["score"] = 2;
  EXPECT_FALSE(DecisionApiClient::ValidateResponse(*request, response).ok());
}

TEST(DecisionApiClientTest, RejectsCatalogHttpErrors) {
  Config config;
  config.api_key = "secret";
  FakeHttpClient http;
  http.get_status = 429;
  http.get_body = "sensitive catalog body";
  DecisionApiClient client(config, &http);

  auto models = client.Models();
  ASSERT_FALSE(models.ok());
  EXPECT_NE(std::string(models.status().message()).find("HTTP 429"), std::string::npos);
  EXPECT_EQ(std::string(models.status().message()).find("sensitive catalog body"), std::string::npos);
  EXPECT_EQ(http.get_count, 1);
}

TEST(DecisionApiClientTest, DoesNotCallTransportForInvalidArgumentsAndPostsOnce) {
  Config config;
  config.api_key = "secret";
  config.model = "configured-model";
  FakeHttpClient http;
  http.post_response.status_code = 200;
  http.post_response.body = json_dump(ValidResponse());
  DecisionApiClient client(config, &http);

  auto invalid = RequestArguments();
  invalid["unexpected"] = true;
  EXPECT_FALSE(client.Decide(invalid).ok());
  EXPECT_EQ(http.post_count, 0);

  auto result = client.Decide(RequestArguments());
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(http.post_count, 1);
  EXPECT_EQ(http.last_url, kDefaultEndpoint);
  EXPECT_NE(http.last_headers.end(),
            std::find(http.last_headers.begin(), http.last_headers.end(), "Authorization: Bearer secret"));
}

TEST(DecisionApiClientTest, ReportsHttpStatusWithoutReplayingOrEchoingResponseBody) {
  Config config;
  config.api_key = "secret";
  config.model = "model-id";
  FakeHttpClient http;
  http.post_response.status_code = 402;
  http.post_response.body = "sensitive upstream body";
  DecisionApiClient client(config, &http);

  auto result = client.Decide(RequestArguments());
  ASSERT_FALSE(result.ok());
  EXPECT_NE(std::string(result.status().message()).find("HTTP 402"), std::string::npos);
  EXPECT_EQ(std::string(result.status().message()).find("sensitive upstream body"), std::string::npos);
  EXPECT_EQ(http.post_count, 1);
}

TEST(DecisionApiClientTest, ReadsModelCatalogOnlyWhenConfigured) {
  Config config;
  config.api_key = "secret";
  FakeHttpClient http;
  http.get_body = R"({"data":[{"id":"~typesafe/jev-latest"}],"total_count":1})";
  DecisionApiClient client(config, &http);
  auto models = client.Models();
  ASSERT_TRUE(models.ok()) << models.status();
  EXPECT_EQ(http.get_count, 1);
  EXPECT_EQ(*json_get<std::string>((*json_at(*models, "data"))[0], "id"), "~typesafe/jev-latest");

  config.models_endpoint = std::nullopt;
  DecisionApiClient no_catalog(config, &http);
  EXPECT_FALSE(no_catalog.Models().ok());
  EXPECT_EQ(http.get_count, 1);
}

}  // namespace
}  // namespace slop::mcp::decision_api
