#include "core/openai_utils.h"

#include <string>
#include <vector>

#include "absl/strings/match.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "core/database.h"
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

TEST(OpenAiUtilsTest, BuildOpenAiToolsNormalizesArraySchemasWithItems) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  auto tools = BuildOpenAiResponsesTools(&db);
  ASSERT_TRUE(tools.is_array());

  nlohmann::json query_db_tool;
  bool found = false;
  for (const auto& t : tools) {
    if (!t.is_object()) continue;
    if (t.value("name", "") == "query_db") {
      query_db_tool = t;
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);
  ASSERT_TRUE(query_db_tool.contains("parameters"));
  auto params = query_db_tool["parameters"]["properties"]["params"];
  ASSERT_TRUE(params.is_object());
  EXPECT_EQ(params.value("type", ""), "array");
  EXPECT_TRUE(params.contains("items"));
}

TEST(OpenAiUtilsTest, BuildOpenAiToolsNormalizesObjectSchemasWithProperties) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  auto tools = BuildOpenAiResponsesTools(&db);
  ASSERT_TRUE(tools.is_array());

  nlohmann::json describe_tool;
  bool found = false;
  for (const auto& t : tools) {
    if (!t.is_object()) continue;
    if (t.value("name", "") == "describe_db") {
      describe_tool = t;
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);
  ASSERT_TRUE(describe_tool.contains("parameters"));
  auto parameters = describe_tool["parameters"];
  ASSERT_TRUE(parameters.is_object());
  EXPECT_EQ(parameters.value("type", ""), "object");
  EXPECT_TRUE(parameters.contains("properties"));
}

TEST(OpenAiUtilsTest, BuildOpenAiToolsExcludesRunJsOnlyOperationalTools) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  auto tools = BuildOpenAiResponsesTools(&db);
  ASSERT_TRUE(tools.is_array());

  auto contains_tool = [&](const std::string& name) {
    for (const auto& t : tools) {
      if (t.is_object() && t.value("name", "") == name) return true;
    }
    return false;
  };

  EXPECT_TRUE(contains_tool("query_db"));
  EXPECT_FALSE(contains_tool("read_file"));
  EXPECT_FALSE(contains_tool("list_directory"));
  EXPECT_FALSE(contains_tool("grep"));
  EXPECT_FALSE(contains_tool("write_file"));
  EXPECT_FALSE(contains_tool("patch_tool"));
  EXPECT_FALSE(contains_tool("execute_bash"));
}

}  // namespace slop
