
#include "acp/server.h"

#include <sstream>

#include <gtest/gtest.h>

namespace slop::acp {
namespace {

TEST(ServerTest, UnknownMethodProducesMethodNotFoundResponse) {
  std::istringstream in(R"({"jsonrpc":"2.0","id":9,"method":"unknown"}
)");
  std::ostringstream out;

  EXPECT_EQ(RunServer(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("\"code\":-32601"), std::string::npos);
  EXPECT_NE(output.find("\"id\":9"), std::string::npos);
}

TEST(ServerTest, InvalidJsonProducesParseErrorResponse) {
  std::istringstream in("not-json\n");
  std::ostringstream out;

  EXPECT_EQ(RunServer(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("\"code\":-32700"), std::string::npos);
}

TEST(ServerTest, InvalidRequestProducesInvalidRequestResponse) {
  std::istringstream in("[]\n");
  std::ostringstream out;

  EXPECT_EQ(RunServer(&in, &out), 0);
  const std::string output = out.str();
  EXPECT_NE(output.find("\"code\":-32600"), std::string::npos);
}

TEST(ServerTest, NotificationProducesNoResponse) {
  std::istringstream in(R"({"jsonrpc":"2.0","method":"noop"}
)");
  std::ostringstream out;

  EXPECT_EQ(RunServer(&in, &out), 0);
  EXPECT_TRUE(out.str().empty());
}

}  // namespace
}  // namespace slop::acp