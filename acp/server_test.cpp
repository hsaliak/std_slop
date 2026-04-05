
#include "acp/server.h"

#include <sstream>

#include <gtest/gtest.h>
#include "core/database.h"

namespace slop::acp {
namespace {

class ServerTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(db_.Init(":memory:").ok()); }

  int Run(std::istringstream* in, std::ostringstream* out) { return RunServer(in, out, &db_); }

  Database db_;
};

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


}  // namespace
}  // namespace slop::acp