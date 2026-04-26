#include "rpc/slop_service.h"

#include <memory>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "app/runtime_bootstrap.h"
#include "core/database.h"
#include "core/http_client.h"
#include "rpc/execution_policy.h"

namespace slop::rpc::v1 {
namespace {

class MockHttpClient final : public slop::HttpClient {
 public:
  MOCK_METHOD(absl::StatusOr<std::string>, Post,
              (const std::string&, const std::string&, const std::vector<std::string>&), (override));
};

ServerRuntimeConfig BaseServerConfig() {
  ServerRuntimeConfig config;
  config.runtime_options.model = "gemini-test";
  config.runtime_options.google_api_key = "dummy-key";
  config.disable_ask_user = true;
  return config;
}

TEST(SlopServiceTest, RunPromptReturnsStructuredSuccessResponse) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  MockHttpClient mock_http;
  RuntimeBootstrapOptions options;
  options.model = "gemini-test";
  options.google_api_key = "dummy-key";

  testing::InSequence sequence;
  EXPECT_CALL(mock_http, Post(testing::_, testing::_, testing::_))
      .WillOnce(testing::Return(R"({"candidates":[{"content":{"parts":[{"text":"rpc ok"}]}}]})"))
      .WillOnce(testing::Return(R"({"candidates":[{"content":{"parts":[{"text":"rpc second"}]}}]})"));

  auto runtime_or = slop::BootstrapRuntime(
      &db, &mock_http, options,
      [](std::shared_ptr<slop::OAuthHandler>*) {},
      [](slop::ToolExecutor&) -> std::vector<std::string> { return {}; },
      [](slop::CommandHandler&) {});
  ASSERT_TRUE(runtime_or.ok()) << runtime_or.status();

  ServerRuntimeConfig server_config = BaseServerConfig();
  ApplyServerExecutionPolicy(*runtime_or->tool_executor, server_config);

  SlopServiceImpl service(server_config, std::move(*runtime_or));
  RunPromptRequest request;
  request.set_prompt("hello rpc");
  request.set_session_id("rpc-session");
  RunPromptResponse response;

  grpc::ServerContext context;
  grpc::Status status = service.RunPrompt(&context, &request, &response);

  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(response.success());
  EXPECT_EQ(response.content(), "rpc ok");
  EXPECT_EQ(response.session_id(), "rpc-session");
  EXPECT_TRUE(response.error_code().empty());
  EXPECT_TRUE(response.error_message().empty());

  auto history_or = db.GetConversationHistory("rpc-session");
  ASSERT_TRUE(history_or.ok()) << history_or.status();
  ASSERT_GE(history_or->size(), 2);
  EXPECT_EQ(history_or->at(history_or->size() - 2).role, "user");
  EXPECT_EQ(history_or->at(history_or->size() - 2).content, "hello rpc");
  EXPECT_EQ(history_or->back().role, "assistant");
  EXPECT_EQ(history_or->back().content, "rpc ok");

  RunPromptRequest second_request;
  second_request.set_prompt("follow up");
  second_request.set_session_id("rpc-session");
  RunPromptResponse second_response;

  grpc::ServerContext second_context;
  status = service.RunPrompt(&second_context, &second_request, &second_response);

  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(second_response.success());
  EXPECT_EQ(second_response.content(), "rpc second");
  EXPECT_EQ(second_response.session_id(), "rpc-session");

  history_or = db.GetConversationHistory("rpc-session");
  ASSERT_TRUE(history_or.ok()) << history_or.status();
  ASSERT_GE(history_or->size(), 4);
  EXPECT_EQ(history_or->at(history_or->size() - 2).role, "user");
  EXPECT_EQ(history_or->at(history_or->size() - 2).content, "follow up");
  EXPECT_EQ(history_or->back().role, "assistant");
  EXPECT_EQ(history_or->back().content, "rpc second");
}

TEST(SlopServiceTest, RunPromptRejectsMissingPromptWithStructuredError) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  MockHttpClient mock_http;
  RuntimeBootstrapOptions options;
  options.model = "gemini-test";
  options.google_api_key = "dummy-key";

  auto runtime_or = slop::BootstrapRuntime(
      &db, &mock_http, options,
      [](std::shared_ptr<slop::OAuthHandler>*) {},
      [](slop::ToolExecutor&) -> std::vector<std::string> { return {}; },
      [](slop::CommandHandler&) {});
  ASSERT_TRUE(runtime_or.ok()) << runtime_or.status();

  ServerRuntimeConfig server_config = BaseServerConfig();
  ApplyServerExecutionPolicy(*runtime_or->tool_executor, server_config);

  SlopServiceImpl service(server_config, std::move(*runtime_or));
  RunPromptRequest request;
  request.set_prompt("   ");
  RunPromptResponse response;

  grpc::ServerContext context;
  grpc::Status status = service.RunPrompt(&context, &request, &response);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_FALSE(response.success());
  EXPECT_EQ(response.error_code(), "INVALID_ARGUMENT");
  EXPECT_THAT(response.error_message(), testing::HasSubstr("prompt is required"));
}

TEST(SlopServiceTest, ConfigureRpcOpenAiOAuthHandlerEnablesOpenAiOAuthHandler) {
  slop::HttpClient http_client;
  std::shared_ptr<slop::OAuthHandler> handler;

  ConfigureRpcOpenAiOAuthHandler(&http_client, &handler);

  ASSERT_NE(handler, nullptr);
  EXPECT_EQ(handler->GetProvider(), slop::OAuthHandler::Provider::kOpenAi);
  EXPECT_TRUE(handler->IsEnabled());
}

}  // namespace
}  // namespace slop::rpc::v1
