
#include "acp/rpc_envelope.h"

#include <string>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop::acp {
namespace {

void ParseRpcEnvelopeNeverCrashes(const std::string& input) {
  auto parsed = ParseRpcRequest(input);
  if (!parsed.ok()) {
    return;
  }

  EXPECT_FALSE(parsed->method.empty());
  if (parsed->id.has_value()) {
    EXPECT_TRUE((*parsed->id).is_number() || (*parsed->id).is_string() || (*parsed->id).is_null());
  }

  auto method_not_found = MakeMethodNotFoundResponse(parsed->id, parsed->method);
  EXPECT_EQ(method_not_found.at("jsonrpc").get<std::string>(), "2.0");
}

FUZZ_TEST(RpcEnvelopeFuzzTest, ParseRpcEnvelopeNeverCrashes);

}  // namespace
}  // namespace slop::acp