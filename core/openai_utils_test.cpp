#include "core/openai_utils.h"

#include <string>
#include <vector>

#include "absl/strings/match.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "core/http_client.h"

namespace slop {

using ::testing::_;
using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::InSequence;
using ::testing::Return;

class MockHttpClient : public HttpClient {
 public:
  MOCK_METHOD(absl::StatusOr<std::string>, Post,
              (const std::string&, const std::string&, const std::vector<std::string>&), (override));
  MOCK_METHOD(absl::StatusOr<std::string>, Get, (const std::string&, const std::vector<std::string>&), (override));
};

TEST(OpenAiUtilsTest, GetOpenAiModelsParsesApiDataShape) {
  MockHttpClient mock_http;
  EXPECT_CALL(mock_http, Get("https://api.openai.com/v1/models", _)).WillOnce(Return(R"({"data":[{"id":"gpt-4o"}]})"));

  auto models_or = GetOpenAiModels(&mock_http, "https://api.openai.com/v1", "test_key");
  ASSERT_TRUE(models_or.ok());
  ASSERT_EQ(models_or->size(), 1);
  EXPECT_EQ((*models_or)[0].id, "gpt-4o");
  EXPECT_EQ((*models_or)[0].name, "gpt-4o");
}

TEST(OpenAiUtilsTest, GetOpenAiModelsParsesCodexModelsShapeAndSetsAccountHeader) {
  MockHttpClient mock_http;
  EXPECT_CALL(mock_http, Get("https://chatgpt.com/backend-api/codex/models?client_version=1.0.0",
                             Contains("ChatGPT-Account-Id: org_test_123")))
      .WillOnce(Return(R"({"models":[{"slug":"gpt-5.3-codex","display_name":"GPT-5.3 Codex"}]})"));

  auto models_or = GetOpenAiModels(&mock_http, "https://chatgpt.com/backend-api/codex", "oauth_token", "org_test_123");
  ASSERT_TRUE(models_or.ok());
  ASSERT_EQ(models_or->size(), 1);
  EXPECT_EQ((*models_or)[0].id, "gpt-5.3-codex");
  EXPECT_EQ((*models_or)[0].name, "GPT-5.3 Codex");
}

TEST(OpenAiUtilsTest, GetOpenAiModelsPaginatesOpenAiDataShape) {
  MockHttpClient mock_http;
  InSequence seq;
  EXPECT_CALL(mock_http, Get("https://api.openai.com/v1/models", _))
      .WillOnce(Return(R"({"data":[{"id":"gpt-5.2"},{"id":"gpt-5.3"}],"has_more":true,"after":"page_2"})"));
  EXPECT_CALL(mock_http, Get("https://api.openai.com/v1/models?after=page_2", _))
      .WillOnce(Return(R"({"data":[{"id":"gpt-5.3"},{"id":"gpt-5.4"}],"has_more":false})"));

  auto models_or = GetOpenAiModels(&mock_http, "https://api.openai.com/v1", "test_key");
  ASSERT_TRUE(models_or.ok());
  ASSERT_EQ(models_or->size(), 3);
  EXPECT_EQ((*models_or)[0].id, "gpt-5.2");
  EXPECT_EQ((*models_or)[1].id, "gpt-5.3");
  EXPECT_EQ((*models_or)[2].id, "gpt-5.4");
}

TEST(OpenAiUtilsTest, GetOpenAiModelsFailsForUnrecognizedSchema) {
  MockHttpClient mock_http;
  EXPECT_CALL(mock_http, Get("https://api.openai.com/v1/models", _)).WillOnce(Return(R"({"unexpected":true})"));

  auto models_or = GetOpenAiModels(&mock_http, "https://api.openai.com/v1", "test_key");
  ASSERT_FALSE(models_or.ok());
  EXPECT_TRUE(absl::StrContains(models_or.status().message(), "Unrecognized models response schema"));
}

}  // namespace slop
