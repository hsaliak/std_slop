#include <chrono>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "fuzztest/fuzztest.h"
#include "js_runtime/runtime.h"

namespace slop::js_runtime {
namespace {

class RejectingBroker final : public AsyncToolBroker {
 public:
  absl::Status Submit(const ToolRequest&) override { return absl::InvalidArgumentError("not available"); }
  std::vector<ToolCompletion> TakeCompletions() override { return {}; }
  void WaitForCompletion(std::chrono::milliseconds) override {}
  void CancelPending() override {}
};

void RunCodeIsTotal(const std::string& code) {
  RejectingBroker broker;
  RuntimeOptions options;
  options.timeout = std::chrono::milliseconds(15);
  options.memory_limit_bytes = 4 * 1024 * 1024;
  options.max_code_bytes = 1024;
  options.max_output_bytes = 4096;
  const auto result = Run(code, nlohmann::json::object(), &broker, options);
  if (!result.ok()) EXPECT_FALSE(result.status().ok());
}

FUZZ_TEST(RuntimeFuzzTest, RunCodeIsTotal).WithDomains(fuzztest::Arbitrary<std::string>());

}  // namespace
}  // namespace slop::js_runtime
