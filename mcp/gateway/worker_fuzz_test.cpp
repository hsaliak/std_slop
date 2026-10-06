#include <string>

#include "gtest/gtest.h"

#include "core/json_utils.h"
#include "fuzztest/fuzztest.h"
#include "mcp/gateway/worker.h"

namespace slop::mcp::gateway {
namespace {

void WorkerCallMessageParsesSafely(const std::string& input) {
  auto json = json_parse(input);
  if (!json.has_value()) return;
  (void)ParseWorkerCallRequest(*json, 1);
}
FUZZ_TEST(WorkerIpcFuzzTest, WorkerCallMessageParsesSafely).WithDomains(fuzztest::Arbitrary<std::string>());

}  // namespace
}  // namespace slop::mcp::gateway
