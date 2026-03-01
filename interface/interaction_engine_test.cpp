#include "interface/interaction_engine.h"

#include "absl/status/statusor.h"
#include "absl/strings/match.h"

#include <fstream>
#include <unistd.h>

#include "core/constants.h"
#include "core/database.h"
#include "core/oauth_handler.h"
#include "core/orchestrator.h"
#include "core/tool_executor.h"
#include "interface/command_handler.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace slop {

class MockHttpClient : public HttpClient {
 public:
  MOCK_METHOD(absl::StatusOr<std::string>, Post,
              (const std::string& url, const std::string& body, const std::vector<std::string>& headers), (override));
};

class InteractionEngineTest : public ::testing::Test {
 protected:
  Database db;
  MockHttpClient mock_http;
  std::unique_ptr<Orchestrator> orchestrator;
  std::unique_ptr<ToolExecutor> tool_executor;
  std::unique_ptr<CommandHandler> cmd_handler;

  void SetUp() override {
    ASSERT_TRUE(db.Init(":memory:").ok());

    auto orch_or = Orchestrator::Builder(&db, &mock_http).Build();
    ASSERT_TRUE(orch_or.ok());
    orchestrator = std::move(*orch_or);

    auto exec_or = ToolExecutor::Create(&db);
    ASSERT_TRUE(exec_or.ok());
    tool_executor = std::move(*exec_or);

    auto dispatcher = std::make_unique<ToolDispatcher>(
        [this](const std::string& name, const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
          return tool_executor->Execute(name, args, cancellation);
        });
    tool_executor->SetDispatcher(std::move(dispatcher));

    auto cmd_or = CommandHandler::Create(&db, orchestrator.get(), nullptr, "", "");
    ASSERT_TRUE(cmd_or.ok());
    cmd_handler = std::move(*cmd_or);
  }

  nlohmann::json GeminiResponse(const std::string& text) {
    nlohmann::json res;
    nlohmann::json candidate;
    candidate["content"]["parts"] = nlohmann::json::array({{{"text", text}}});
    res["candidates"] = nlohmann::json::array({candidate});
    return res;
  }

  nlohmann::json GeminiToolCall(const std::string& name, const nlohmann::json& args) {
    nlohmann::json res;
    nlohmann::json candidate;
    nlohmann::json functionCall;
    functionCall["name"] = name;
    functionCall["args"] = args;
    candidate["content"]["parts"] = nlohmann::json::array({{{"functionCall", functionCall}}});
    res["candidates"] = nlohmann::json::array({candidate});
    return res;
  }
};

TEST_F(InteractionEngineTest, QueryIsolationTest) {
  InteractionEngine engine(db, *orchestrator, *cmd_handler, *tool_executor->dispatcher(), *tool_executor, mock_http,
                           nullptr);

  InteractionEngine::Config config;
  config.silent = true;

  EXPECT_CALL(mock_http, Post(testing::_, testing::_, testing::_))
      .WillOnce(testing::Return(GeminiResponse("The answer is 42").dump()));

  auto result = engine.Query("What is the answer?", config);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(*result, "The answer is 42");

  auto history = db.GetConversationHistory("query");
  ASSERT_TRUE(history.ok());
  EXPECT_TRUE(history->empty());
}

TEST_F(InteractionEngineTest, QueryWithNestedToolsTest) {
  InteractionEngine engine(db, *orchestrator, *cmd_handler, *tool_executor->dispatcher(), *tool_executor, mock_http,
                           nullptr);

  InteractionEngine::Config config;
  config.silent = true;

  EXPECT_CALL(mock_http, Post(testing::_, testing::_, testing::_))
      .WillOnce(testing::Return(GeminiToolCall("query_db", {{"sql", "SELECT 1"}}).dump()))
      .WillOnce(testing::Return(GeminiResponse("The result is 1").dump()));

  auto result = engine.Query("Query something", config);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(*result, "The result is 1");
}

TEST_F(InteractionEngineTest, ErrorHandlingTest) {
  InteractionEngine engine(db, *orchestrator, *cmd_handler, *tool_executor->dispatcher(), *tool_executor, mock_http,
                           nullptr);

  InteractionEngine::Config config;
  config.silent = true;

  EXPECT_CALL(mock_http, Post(testing::_, testing::_, testing::_))
      .WillOnce(testing::Return(absl::InternalError("Network Error")));

  auto result = engine.Query("This will fail", config);
  EXPECT_FALSE(result.ok());
}

TEST_F(InteractionEngineTest, OpenAiOAuthUsesCodexEndpointAndAccountHeader) {
  char temp_path[] = "/tmp/slop_openai_token_ie_XXXXXX";
  int fd = mkstemp(temp_path);
  close(fd);

  {
    std::ofstream f(temp_path);
    f << R"({
  "access_token": "eyJhbGciOiJub25lIiwidHlwIjoiSldUIn0.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoib3JnX3Rlc3RfMTIzIn19.c2ln",
  "refresh_token": "fake_refresh",
  "expiry_time": 9999999999
})";
  }

  auto orch_or = Orchestrator::Builder(&db, &mock_http)
                     .WithProvider(Orchestrator::Provider::OPENAI)
                     .WithModel("gpt-5.3-codex")
                     .WithBaseUrl(kOpenAiChatGptCodexBaseUrl)
                     .WithOpenAiApiStyle(Orchestrator::OpenAiApiStyle::RESPONSES)
                     .Build();
  ASSERT_TRUE(orch_or.ok());
  orchestrator = std::move(*orch_or);

  auto cmd_or = CommandHandler::Create(&db, orchestrator.get(), nullptr, "", "");
  ASSERT_TRUE(cmd_or.ok());
  cmd_handler = std::move(*cmd_or);

  auto oauth_handler = std::make_shared<OAuthHandler>(&mock_http, OAuthHandler::Provider::kOpenAi);
  oauth_handler->SetTokenPath(temp_path);
  oauth_handler->SetEnabled(true);

  InteractionEngine engine(db, *orchestrator, *cmd_handler, *tool_executor->dispatcher(), *tool_executor, mock_http,
                           oauth_handler);

  InteractionEngine::Config config;
  config.silent = true;
  config.openai_oauth = true;
  config.use_responses = true;
  config.openai_base_url = kOpenAiChatGptCodexBaseUrl;

  EXPECT_CALL(mock_http, Post(testing::Eq("https://chatgpt.com/backend-api/codex/responses"), testing::_, testing::_))
      .WillOnce(testing::DoAll(
          testing::WithArg<2>([](const std::vector<std::string>& headers) {
            bool has_auth = false;
            bool has_account = false;
            for (const auto& header : headers) {
              has_auth = has_auth || absl::StartsWith(header, "Authorization: Bearer ");
              has_account = has_account || header == "ChatGPT-Account-Id: org_test_123";
            }
            EXPECT_TRUE(has_auth);
            EXPECT_TRUE(has_account);
          }),
          testing::Return(R"({"output":[{"type":"message","content":[{"type":"output_text","text":"ok"}]}]})")));

  auto result = engine.Query("ping", config);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(*result, "ok");

  unlink(temp_path);
}

}  // namespace slop
