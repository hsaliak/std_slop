
#include "acp/engine_adapter.h"

#include <gtest/gtest.h>

namespace slop::acp {
namespace {

TEST(EngineAdapterTest, ParseSessionPromptParamsValid) {
  nlohmann::json params = {
      {"sessionId", "acp_1"},
      {"prompt", "hello"},
  };

  auto parsed_or = ParseSessionPromptParams(params);
  ASSERT_TRUE(parsed_or.ok());
  EXPECT_EQ(parsed_or->session_id, "acp_1");
  EXPECT_EQ(parsed_or->prompt, "hello");
}

TEST(EngineAdapterTest, ParseSessionPromptParamsRejectsNonObject) {
  auto parsed_or = ParseSessionPromptParams(nlohmann::json::array());
  ASSERT_FALSE(parsed_or.ok());
  EXPECT_EQ(parsed_or.status().message(), "session_prompt_params_must_be_object");
}

TEST(EngineAdapterTest, ParseSessionPromptParamsRejectsMissingSessionId) {
  nlohmann::json params = {
      {"prompt", "hello"},
  };

  auto parsed_or = ParseSessionPromptParams(params);
  ASSERT_FALSE(parsed_or.ok());
  EXPECT_EQ(parsed_or.status().message(), "session_prompt_session_id_required");
}

TEST(EngineAdapterTest, ParseSessionPromptParamsRejectsMalformedSessionId) {
  nlohmann::json params = {
      {"sessionId", "bad id"},
      {"prompt", "hello"},
  };

  auto parsed_or = ParseSessionPromptParams(params);
  ASSERT_FALSE(parsed_or.ok());
  EXPECT_EQ(parsed_or.status().message(), "session_prompt_session_id_invalid");
}

TEST(EngineAdapterTest, ParseSessionPromptParamsRejectsMissingPrompt) {
  nlohmann::json params = {
      {"sessionId", "acp_1"},
  };

  auto parsed_or = ParseSessionPromptParams(params);
  ASSERT_FALSE(parsed_or.ok());
  EXPECT_EQ(parsed_or.status().message(), "session_prompt_prompt_must_be_string");
}

TEST(EngineAdapterTest, ParseSessionPromptParamsRejectsEmptyPrompt) {
  nlohmann::json params = {
      {"sessionId", "acp_1"},
      {"prompt", ""},
  };

  auto parsed_or = ParseSessionPromptParams(params);
  ASSERT_FALSE(parsed_or.ok());
  EXPECT_EQ(parsed_or.status().message(), "session_prompt_prompt_required");
}

TEST(EngineAdapterTest, ParseSessionCancelParamsValid) {
  nlohmann::json params = {
      {"sessionId", "acp_1"},
  };

  auto parsed_or = ParseSessionCancelParams(params);
  ASSERT_TRUE(parsed_or.ok());
  EXPECT_EQ(parsed_or->session_id, "acp_1");
}

TEST(EngineAdapterTest, ParseSessionCancelParamsRejectsMissingSessionId) {
  auto parsed_or = ParseSessionCancelParams(nlohmann::json::object());
  ASSERT_FALSE(parsed_or.ok());
  EXPECT_EQ(parsed_or.status().message(), "session_cancel_session_id_required");
}

TEST(EngineAdapterTest, ParseSessionCancelParamsRejectsMalformedSessionId) {
  nlohmann::json params = {
      {"sessionId", "bad id"},
  };

  auto parsed_or = ParseSessionCancelParams(params);
  ASSERT_FALSE(parsed_or.ok());
  EXPECT_EQ(parsed_or.status().message(), "session_cancel_session_id_invalid");
}

TEST(EngineAdapterTest, ExecuteSessionPromptRunsExecutorForExistingSession) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.Execute("INSERT INTO sessions (id) VALUES ('acp_1')").ok());

  SessionPromptRequest req;
  req.session_id = "acp_1";
  req.prompt = "hello";

  auto result_or = ExecuteSessionPrompt(
      &db, req, [](const std::string& session_id, const std::string& prompt,
                   std::shared_ptr<CancellationRequest>) -> absl::StatusOr<std::string> {
        return session_id + "::" + prompt;
      }, nullptr);
  ASSERT_TRUE(result_or.ok());
  EXPECT_EQ(result_or->at("sessionId").get<std::string>(), "acp_1");
  EXPECT_EQ(result_or->at("content").get<std::string>(), "acp_1::hello");
}

TEST(EngineAdapterTest, ExecuteSessionPromptRejectsMissingSession) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  SessionPromptRequest req;
  req.session_id = "missing";
  req.prompt = "hello";

  auto result_or = ExecuteSessionPrompt(
      &db, req, [](const std::string& session_id, const std::string& prompt,
                   std::shared_ptr<CancellationRequest>) -> absl::StatusOr<std::string> {
        return session_id + "::" + prompt;
      }, nullptr);
  ASSERT_FALSE(result_or.ok());
  EXPECT_EQ(result_or.status().message(), "session_prompt_session_not_found");
}

TEST(EngineAdapterTest, ExecuteSessionPromptMapsEngineFailure) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.Execute("INSERT INTO sessions (id) VALUES ('acp_1')").ok());

  SessionPromptRequest req;
  req.session_id = "acp_1";
  req.prompt = "hello";

  auto result_or = ExecuteSessionPrompt(
      &db, req,
      [](const std::string&, const std::string&, std::shared_ptr<CancellationRequest>) -> absl::StatusOr<std::string> {
        return absl::InternalError("boom");
      }, nullptr);
  ASSERT_FALSE(result_or.ok());
  EXPECT_EQ(result_or.status().message(), "session_prompt_engine_failure");
}

TEST(EngineAdapterTest, ExecuteSessionPromptMapsCancelledFailure) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.Execute("INSERT INTO sessions (id) VALUES ('acp_1')").ok());

  SessionPromptRequest req;
  req.session_id = "acp_1";
  req.prompt = "hello";

  auto result_or = ExecuteSessionPrompt(&db, req,
      [](const std::string&, const std::string&, std::shared_ptr<CancellationRequest>) -> absl::StatusOr<std::string> {
        return absl::CancelledError("cancelled");
      }, nullptr);
  ASSERT_TRUE(result_or.ok());
  EXPECT_EQ(result_or->at("stopReason").get<std::string>(), "cancelled");
  EXPECT_EQ(result_or->at("sessionId").get<std::string>(), "acp_1");
}

TEST(EngineAdapterTest, ExecuteSessionPromptPassesCancellationIntoExecutor) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.Execute("INSERT INTO sessions (id) VALUES ('acp_1')").ok());

  SessionPromptRequest req;
  req.session_id = "acp_1";
  req.prompt = "hello";

  auto result_or = ExecuteSessionPrompt(&db, req,
      [](const std::string&, const std::string&, std::shared_ptr<CancellationRequest> cancellation)
          -> absl::StatusOr<std::string> {
        if (!cancellation) {
          return absl::InternalError("missing_cancellation");
        }
        return std::string("ok");
      }, nullptr);
  ASSERT_TRUE(result_or.ok());
  EXPECT_EQ(result_or->at("content").get<std::string>(), "ok");
}

TEST(EngineAdapterTest, ExecuteSessionPromptPassesThroughInputErrors) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.Execute("INSERT INTO sessions (id) VALUES ('acp_1')").ok());

  SessionPromptRequest req;
  req.session_id = "acp_1";
  req.prompt = "hello";

  auto result_or = ExecuteSessionPrompt(
      &db, req, [](const std::string&, const std::string&, std::shared_ptr<CancellationRequest>) -> absl::StatusOr<std::string> {
        return absl::InvalidArgumentError("input_bad");
      }, nullptr);
  ASSERT_FALSE(result_or.ok());
  EXPECT_EQ(result_or.status().message(), "input_bad");
}

}  // namespace
}  // namespace slop::acp