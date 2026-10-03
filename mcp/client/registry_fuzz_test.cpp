#include <string>

#include "absl/status/status.h"
#include "gtest/gtest.h"

#include "fuzztest/fuzztest.h"
#include "mcp/client/registry.h"
#include "mcp/client/stdio_transport.h"

namespace slop::mcp {
namespace {

void ServerArgsJsonNeverCrashes(const std::string& input) {
  auto args = ParseServerArgsJson(input);
  if (!args.ok()) return;
  (void)ValidateStdioProcessSpec("/test/server", *args);
}
FUZZ_TEST(RegistryFuzzTest, ServerArgsJsonNeverCrashes);

TEST(RegistryFuzzTest, RegressionSeeds) {
  ServerArgsJsonNeverCrashes("[]");
  ServerArgsJsonNeverCrashes(R"(["argument with spaces", "$HOME", "#;"])");
  ServerArgsJsonNeverCrashes(R"(["\u0000"])");
  ServerArgsJsonNeverCrashes(R"({"argument":"not an array"})");
  ServerArgsJsonNeverCrashes("[");
}

}  // namespace
}  // namespace slop::mcp
