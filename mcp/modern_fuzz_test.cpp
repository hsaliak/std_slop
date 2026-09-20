#include <string>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "core/json_utils.h"
#include "fuzztest/fuzztest.h"
#include "mcp/modern.h"

namespace slop::mcp::v2026_07_28 {
namespace {

void ModernPayloadNeverCrashes(const std::string& input) {
  const auto value = json_parse(input);
  if (!value) return;
  (void)ParseDiscovery(*value);
  (void)ParseResult(*value);
  (void)ParseToolsList(*value);
  (void)ParseToolCallResult(*value);
}
FUZZ_TEST(ModernCodecFuzzTest, ModernPayloadNeverCrashes);

TEST(ModernCodecFuzzTest, RegressionSeeds) {
  ModernPayloadNeverCrashes(R"({"supportedVersions":["2026-07-28"],"capabilities":{}})");
  ModernPayloadNeverCrashes(R"({"resultType":"input_required","requestState":{"step":1},"content":[]})");
  ModernPayloadNeverCrashes(R"({"resultType":"unknown"})");
}

}  // namespace
}  // namespace slop::mcp::v2026_07_28
