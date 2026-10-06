#include "mcp/gateway/worker.h"

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace slop::mcp::gateway {
namespace {

TEST(WorkerIpcTest, ParsesBoundedCallRequest) {
  const nlohmann::json message = {{"type", "call_request"}, {"runId", 7},     {"callId", 2},
                                  {"server", "echo"},       {"tool", "echo"}, {"arguments", {{"text", "hello"}}}};

  auto request = ParseWorkerCallRequest(message, 7);

  ASSERT_TRUE(request.ok()) << request.status();
  EXPECT_EQ(request->run_id, 7);
  EXPECT_EQ(request->call_id, 2);
  EXPECT_EQ(request->server, "echo");
  EXPECT_EQ(request->tool, "echo");
  EXPECT_EQ(request->arguments["text"], "hello");
}

TEST(WorkerIpcTest, RejectsMismatchedAndInvalidCallRequests) {
  const nlohmann::json valid = {{"type", "call_request"}, {"runId", 7},     {"callId", 2},
                                {"server", "echo"},       {"tool", "echo"}, {"arguments", nlohmann::json::object()}};
  EXPECT_FALSE(ParseWorkerCallRequest(valid, 8).ok());

  nlohmann::json invalid = valid;
  invalid["callId"] = 0;
  EXPECT_FALSE(ParseWorkerCallRequest(invalid, 7).ok());
  invalid = valid;
  invalid["endpoint"] = "https://attacker.test";
  EXPECT_FALSE(ParseWorkerCallRequest(invalid, 7).ok());
  invalid = valid;
  invalid["arguments"] = nlohmann::json::array();
  EXPECT_FALSE(ParseWorkerCallRequest(invalid, 7).ok());
  invalid = valid;
  invalid["server"] = std::string(65, 'x');
  EXPECT_FALSE(ParseWorkerCallRequest(invalid, 7).ok());
}

}  // namespace
}  // namespace slop::mcp::gateway
