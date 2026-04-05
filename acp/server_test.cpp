
#include "acp/server.h"

#include <chrono>
#include <sstream>
#include <thread>

#include <gtest/gtest.h>
#include "core/database.h"

namespace slop::acp {
namespace {

class ServerTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(db_.Init(":memory:").ok()); }

  static absl::StatusOr<std::string> PromptExec(const std::string& session_id, const std::string& prompt,
                                                std::shared_ptr<CancellationRequest>) {
    return session_id + "::" + prompt;
  }

  static absl::StatusOr<std::string> SlowPromptExec(const std::string& session_id, const std::string& prompt,
                                                    std::shared_ptr<CancellationRequest> cancellation) {
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

absl::StatusOr<std::string> FailingPromptExec(const std::string&, const std::string&, std::shared_ptr<CancellationRequest>) {
  return absl::InternalError("boom");
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
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1","capabilities":{}}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("\"protocolVersion\":\"1\""), std::string::npos);
  EXPECT_NE(output.find("\"session\""), std::string::npos);
}

TEST_F(ServerTest, InitializeRejectsUnsupportedVersion) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2","capabilities":{}}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("\"code\":-32600"), std::string::npos);
  EXPECT_NE(output.find("unsupported_protocol_version"), std::string::npos);
}

TEST_F(ServerTest, SessionNewSucceedsAfterInitialize) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1","capabilities":{}}}
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
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1","capabilities":{}}}
{"jsonrpc":"2.0","id":2,"method":"session/new","params":{"sessionId":"bad id"}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("session_new_session_id_invalid"), std::string::npos);
}

TEST_F(ServerTest, SessionPromptSucceedsAfterInitializeAndSessionCreate) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1","capabilities":{}}}
{"jsonrpc":"2.0","id":2,"method":"session/new","params":{"sessionId":"acp_1"}}
{"jsonrpc":"2.0","id":3,"method":"session/prompt","params":{"sessionId":"acp_1","prompt":"hello"}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("\"content\":\"acp_1::hello\""), std::string::npos);
  EXPECT_NE(output.find("\"sessionId\":\"acp_1\""), std::string::npos);
}

TEST_F(ServerTest, SessionPromptMissingSessionRejected) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1","capabilities":{}}}
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
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1","capabilities":{}}}
{"jsonrpc":"2.0","id":2,"method":"session/prompt","params":{"sessionId":"bad id","prompt":"hello"}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("session_prompt_session_id_invalid"), std::string::npos);
}

TEST_F(ServerTest, SessionPromptEngineFailureReturnsInternalErrorCode) {
  ASSERT_TRUE(db_.Execute("INSERT INTO sessions (id) VALUES ('acp_1')").ok());
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1","capabilities":{}}}
{"jsonrpc":"2.0","id":2,"method":"session/prompt","params":{"sessionId":"acp_1","prompt":"hello"}}
)");
  std::ostringstream out;

  EXPECT_EQ(RunWithExecutor(&in, &out, FailingPromptExec), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("\"code\":-32603"), std::string::npos);
  EXPECT_NE(output.find("session_prompt_engine_failure"), std::string::npos);
}

TEST_F(ServerTest, SessionCancelUnknownRequestReturnsDeterministicError) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1","capabilities":{}}}
{"jsonrpc":"2.0","id":2,"method":"session/cancel","params":{"sessionId":"acp_1"}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("session_cancel_request_not_found"), std::string::npos);
}

TEST_F(ServerTest, SessionCancelCanCancelActivePromptAndReturnsCancelledResult) {
  ASSERT_TRUE(db_.Execute("INSERT INTO sessions (id) VALUES ('acp_1')").ok());

  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1","capabilities":{}}}
{"jsonrpc":"2.0","id":2,"method":"session/prompt","params":{"sessionId":"acp_1","prompt":"hello"}}
{"jsonrpc":"2.0","id":3,"method":"session/cancel","params":{"sessionId":"acp_1"}}
)");
  std::ostringstream out;

  EXPECT_EQ(RunWithSlowExecutor(&in, &out), 0);

  const std::string output = out.str();
  EXPECT_NE(output.find("\"cancelled\":true"), std::string::npos);
  EXPECT_NE(output.find("\"stopReason\":\"cancelled\""), std::string::npos);
}

TEST_F(ServerTest, SessionCancelMalformedSessionIdRejected) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1","capabilities":{}}}
{"jsonrpc":"2.0","id":2,"method":"session/cancel","params":{"sessionId":"bad id"}}
)");
  std::ostringstream out;

  EXPECT_EQ(Run(&in, &out), 0);
  EXPECT_NE(out.str().find("session_cancel_session_id_invalid"), std::string::npos);
}


}  // namespace
}  // namespace slop::acp