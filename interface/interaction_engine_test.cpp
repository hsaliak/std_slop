#include "interface/interaction_engine.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <nlohmann/json.hpp>
#include "absl/status/statusor.h"
#include "core/database.h"
#include "core/orchestrator.h"
#include "core/tool_executor.h"
#include "interface/command_handler.h"

namespace slop {

class MockHttpClient : public HttpClient {
 public:
  MOCK_METHOD(absl::StatusOr<std::string>, Post,
              (const std::string& url, const std::string& body,
               const std::vector<std::string>& headers),
              (override));
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
        [this](const std::string& name, const nlohmann::json& args,
               std::shared_ptr<CancellationRequest> cancellation) {
          return tool_executor->Execute(name, args, cancellation);
        },
        1);
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
  InteractionEngine engine(db, *orchestrator, *cmd_handler, *tool_executor->dispatcher(),
                           *tool_executor, mock_http, nullptr);
  
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
  InteractionEngine engine(db, *orchestrator, *cmd_handler, *tool_executor->dispatcher(),
                           *tool_executor, mock_http, nullptr);
  
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
  InteractionEngine engine(db, *orchestrator, *cmd_handler, *tool_executor->dispatcher(),
                           *tool_executor, mock_http, nullptr);
  
  InteractionEngine::Config config;
  config.silent = true;

  EXPECT_CALL(mock_http, Post(testing::_, testing::_, testing::_))
      .WillOnce(testing::Return(absl::InternalError("Network Error")));

  auto result = engine.Query("This will fail", config);
  EXPECT_FALSE(result.ok());
}

} // namespace slop
