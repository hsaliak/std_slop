
#include "acp/server.h"

#include <chrono>
#include <sstream>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_replace.h"
#include "core/database.h"

namespace slop::acp {
namespace {

class ServerTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(db_.Init(":memory:").ok()); }

  static absl::StatusOr<std::string> PromptExec(const std::string& session_id, const std::string& prompt,
                                                std::shared_ptr<CancellationRequest>, const SessionUpdateWriter&) {
    return session_id + "::" + prompt;
  }

  static absl::StatusOr<std::string> SlowPromptExec(const std::string& session_id, const std::string& prompt,
                                                    std::shared_ptr<CancellationRequest> cancellation,
                                                    const SessionUpdateWriter&) {
    for (int i = 0; i < 200; ++i) {
      if (cancellation && cancellation->IsCancelled()) {
        return absl::CancelledError("cancelled");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return session_id + "::" + prompt;
  }

  int Run(std::istringstream* in, std::ostringstream* out) {
    return RunServer(in, out, &db_, PromptExec);
  }

  int RunWithSlowExecutor(std::istringstream* in, std::ostringstream* out) {
    return RunServer(in, out, &db_, SlowPromptExec);
  }

  int RunWithExecutor(std::istringstream* in, std::ostringstream* out, PromptExecutor executor) {
    return RunServer(in, out, &db_, executor);
  }

  Database db_;
};

absl::StatusOr<std::string> FailingPromptExec(const std::string&, const std::string&, std::shared_ptr<CancellationRequest>,
                                              const SessionUpdateWriter&) {
  return absl::InternalError("boom");
}

absl::StatusOr<std::string> StreamingPromptExec(const std::string& session_id, const std::string& prompt,
                                                std::shared_ptr<CancellationRequest>,
                                                const SessionUpdateWriter& session_update_writer) {
  session_update_writer(MakeToolCallUpdateNotification(session_id, "call_1", "echo", "in_progress", "{\"x\":1}"));
  session_update_writer(MakeToolCallUpdateNotification(session_id, "call_1", "echo", "completed", "done"));
  session_update_writer(MakeAgentMessageChunkNotification(session_id, absl::StrCat("partial:", prompt)));
  return absl::StrCat(session_id, "::", prompt);
}

TEST_F(ServerTest, UnknownMethodProducesMethodNotFoundResponse) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":9,"method":"unknown"}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("\"code\":-32601"), std::string::npos);
  EXPECT_NE(output.find("\"id\":9"), std::string::npos);
}

TEST_F(ServerTest, InvalidJsonProducesParseErrorResponse) {
  std::istringstream in("not-json\n");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("\"code\":-32700"), std::string::npos);
}

TEST_F(ServerTest, InvalidRequestProducesInvalidRequestResponse) {
  std::istringstream in("[]\n");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("\"code\":-32600"), std::string::npos);
}

TEST_F(ServerTest, NotificationProducesNoResponse) {
  std::istringstream in(R"({"jsonrpc":"2.0","method":"noop"}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  EXPECT_TRUE(out.str().empty());
}

TEST_F(ServerTest, InitializeSucceedsWithStableVersion) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"capabilities":{}}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("\"protocolVersion\":1"), std::string::npos);
  EXPECT_NE(output.find("\"session\""), std::string::npos);
}

TEST_F(ServerTest, InitializeRejectsUnsupportedVersion) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":2,"capabilities":{}}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("\"code\":-32600"), std::string::npos);
  EXPECT_NE(output.find("unsupported_protocol_version"), std::string::npos);
}

TEST_F(ServerTest, SessionNewSucceedsAfterInitialize) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"capabilities":{}}}
{"jsonrpc":"2.0","id":2,"method":"session/new","params":{"sessionId":"acp_1"}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("\"sessionId\":\"acp_1\""), std::string::npos);
  auto session_or = db_.GetContextSettings("acp_1");
  EXPECT_TRUE(session_or.ok());
}

TEST_F(ServerTest, SessionNewBeforeInitializeRejected) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":2,"method":"session/new","params":{"sessionId":"acp_1"}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("initialize_required"), std::string::npos);
}

TEST_F(ServerTest, SessionNewRejectsMalformedId) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"capabilities":{}}}
{"jsonrpc":"2.0","id":2,"method":"session/new","params":{"sessionId":"bad id"}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("session_new_session_id_invalid"), std::string::npos);
}

TEST_F(ServerTest, SessionPromptSucceedsAfterInitializeAndSessionCreate) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"capabilities":{}}}
{"jsonrpc":"2.0","id":2,"method":"session/new","params":{"sessionId":"acp_1"}}
{"jsonrpc":"2.0","id":3,"method":"session/prompt","params":{"sessionId":"acp_1","prompt":"hello"}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("\"method\":\"session/update\""), std::string::npos);
  EXPECT_NE(output.find("\"sessionUpdate\":\"agent_thought_chunk\""), std::string::npos);
  EXPECT_NE(output.find("\"text\":\"accepted\""), std::string::npos);
  EXPECT_NE(output.find("\"text\":\"started\""), std::string::npos);
  EXPECT_EQ(output.find("\"text\":\"acp_1::hello\""), std::string::npos);
  EXPECT_NE(output.find("\"sessionId\":\"acp_1\""), std::string::npos);
  EXPECT_NE(output.find("\"stopReason\":\"end_turn\""), std::string::npos);
}

TEST_F(ServerTest, SessionPromptMissingSessionRejected) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"capabilities":{}}}
{"jsonrpc":"2.0","id":3,"method":"session/prompt","params":{"sessionId":"missing","prompt":"hello"}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("session_prompt_session_not_found"), std::string::npos);
}

TEST_F(ServerTest, SessionPromptBeforeInitializeRejected) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"session/prompt","params":{"sessionId":"acp_1","prompt":"hello"}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("initialize_required"), std::string::npos);
}

TEST_F(ServerTest, SessionPromptRejectsMalformedSessionId) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"capabilities":{}}}
{"jsonrpc":"2.0","id":2,"method":"session/prompt","params":{"sessionId":"bad id","prompt":"hello"}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("session_prompt_session_id_invalid"), std::string::npos);
}

TEST_F(ServerTest, SessionPromptEngineFailureReturnsInternalErrorCode) {
  ASSERT_TRUE(db_.Execute("INSERT INTO sessions (id) VALUES ('acp_1')").ok());
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"capabilities":{}}}
{"jsonrpc":"2.0","id":2,"method":"session/prompt","params":{"sessionId":"acp_1","prompt":"hello"}}
)");
  std::ostringstream out;

  EXPECT_EQ(RunWithExecutor(&in, &out, FailingPromptExec), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("\"code\":-32603"), std::string::npos);
  EXPECT_NE(output.find("\"method\":\"session/update\""), std::string::npos);
  const size_t completed_idx = output.find("\"text\":\"completed\"");
  const size_t error_idx = output.find("\"code\":-32603");
  ASSERT_NE(completed_idx, std::string::npos);
  ASSERT_NE(error_idx, std::string::npos);
  EXPECT_LT(completed_idx, error_idx);
  EXPECT_NE(output.find("session_prompt_engine_failure"), std::string::npos);
}

TEST_F(ServerTest, SessionCancelUnknownRequestReturnsDeterministicError) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"capabilities":{}}}
{"jsonrpc":"2.0","id":2,"method":"session/cancel","params":{"sessionId":"acp_1"}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("session_cancel_request_not_found"), std::string::npos);
}

TEST_F(ServerTest, SessionCancelCanCancelActivePromptAndReturnsCancelledResult) {
  ASSERT_TRUE(db_.Execute("INSERT INTO sessions (id) VALUES ('acp_1')").ok());

  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"capabilities":{}}}
{"jsonrpc":"2.0","id":2,"method":"session/prompt","params":{"sessionId":"acp_1","prompt":"hello"}}
{"jsonrpc":"2.0","id":3,"method":"session/cancel","params":{"sessionId":"acp_1"}}
)");
  std::ostringstream out;

  EXPECT_EQ(RunWithSlowExecutor(&in, &out), 0);

  const std::string output = out.str();
  EXPECT_NE(output.find("\"sessionUpdate\":\"agent_thought_chunk\""), std::string::npos);
  EXPECT_NE(output.find("\"text\":\"accepted\""), std::string::npos);
  EXPECT_NE(output.find("\"text\":\"started\""), std::string::npos);
  EXPECT_NE(output.find("\"text\":\"cancelled\""), std::string::npos);
  EXPECT_NE(output.find("\"cancelled\":true"), std::string::npos);
  EXPECT_NE(output.find("\"stopReason\":\"cancelled\""), std::string::npos);
}

TEST_F(ServerTest, SessionPromptResultMatchesAcpPromptResponseShape) {
  ASSERT_TRUE(db_.Execute("INSERT INTO sessions (id) VALUES ('acp_1')").ok());

  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"capabilities":{}}}
{"jsonrpc":"2.0","id":3,"method":"session/prompt","params":{"sessionId":"acp_1","prompt":"hello"}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("\"id\":3"), std::string::npos);
  EXPECT_NE(output.find("\"stopReason\":\"end_turn\""), std::string::npos);
  EXPECT_EQ(output.find("\"content\":\"acp_1::hello\""), std::string::npos);
}

TEST_F(ServerTest, SessionPromptRejectsSecondInFlightPromptForSameSession) {
  ASSERT_TRUE(db_.Execute("INSERT INTO sessions (id) VALUES ('acp_1')").ok());

  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"capabilities":{}}}
{"jsonrpc":"2.0","id":2,"method":"session/prompt","params":{"sessionId":"acp_1","prompt":"hello"}}
{"jsonrpc":"2.0","id":3,"method":"session/prompt","params":{"sessionId":"acp_1","prompt":"again"}}
)");
  std::ostringstream out;

  EXPECT_EQ(RunWithSlowExecutor(&in, &out), 0);
  EXPECT_NE(out.str().find("session_prompt_request_already_in_flight"), std::string::npos);
}

TEST_F(ServerTest, SessionPromptPlainTextStillFlowsThroughExecutor) {
  ASSERT_TRUE(db_.Execute("INSERT INTO sessions (id) VALUES ('acp_1')").ok());

  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"capabilities":{}}}
{"jsonrpc":"2.0","id":2,"method":"session/prompt","params":{"sessionId":"acp_1","prompt":"hello"}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_EQ(output.find("\"text\":\"acp_1::hello\""), std::string::npos);
  EXPECT_NE(output.find("\"stopReason\":\"end_turn\""), std::string::npos);
}

TEST_F(ServerTest, SessionPromptBlockedCommandsTableDriven) {
  ASSERT_TRUE(db_.Execute("INSERT INTO sessions (id) VALUES ('acp_1')").ok());

  const std::vector<std::string> blocked_prompts = {
      "/exec ls",
      "   /review mail",
      "/session remove acp_1",
      "/message list",
      "/scratchpad show",
      "/feedback anything",
  };

  for (const auto& prompt : blocked_prompts) {
    const std::string escaped_prompt = absl::StrReplaceAll(prompt, {{"\\", "\\\\"}, {"\"", "\\\""}});
    std::istringstream in(absl::StrFormat(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":1,\"capabilities\":{}}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"session/prompt\",\"params\":{\"sessionId\":\"acp_1\",\"prompt\":\"%s\"}}\n",
        escaped_prompt));
    std::ostringstream out;
    EXPECT_EQ(Run(&in, &out), 0);
    const std::string output = out.str();

    const size_t result_idx = output.find("\"id\":2");
    ASSERT_NE(result_idx, std::string::npos) << prompt;
    EXPECT_NE(output.find("\"stopReason\":\"end_turn\""), std::string::npos) << prompt;
  }
}

TEST_F(ServerTest, SessionCancelMalformedSessionIdRejected) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"capabilities":{}}}
{"jsonrpc":"2.0","id":2,"method":"session/cancel","params":{"sessionId":"bad id"}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  EXPECT_NE(out.str().find("session_cancel_session_id_invalid"), std::string::npos);
}

TEST_F(ServerTest, SessionPromptStreamsToolAndAssistantUpdatesBeforeResult) {
  ASSERT_TRUE(db_.Execute("INSERT INTO sessions (id) VALUES ('acp_1')").ok());
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"capabilities":{}}}
{"jsonrpc":"2.0","id":2,"method":"session/prompt","params":{"sessionId":"acp_1","prompt":"stream me"}}
)");
  std::ostringstream out;

  EXPECT_EQ(RunWithExecutor(&in, &out, StreamingPromptExec), 0);
  const std::string output = out.str();

  const size_t tool_idx = output.find("\"sessionUpdate\":\"tool_call_update\"");
  const size_t assistant_idx = output.find("partial:stream me");
  const size_t duplicate_assistant_idx = output.find("partial:stream me", assistant_idx + 1);
  const size_t result_idx = output.find("\"id\":2");
  ASSERT_NE(tool_idx, std::string::npos);
  ASSERT_NE(assistant_idx, std::string::npos);
  EXPECT_EQ(duplicate_assistant_idx, std::string::npos) << output;
  ASSERT_NE(result_idx, std::string::npos);
  EXPECT_LT(tool_idx, result_idx);
  EXPECT_LT(assistant_idx, result_idx);
  EXPECT_NE(output.find("\"toolCallId\":\"call_1\""), std::string::npos);
  EXPECT_NE(output.find("\"status\":\"in_progress\""), std::string::npos);
  EXPECT_NE(output.find("\"status\":\"completed\""), std::string::npos);
}


}  // namespace
}  // namespace slop::acp