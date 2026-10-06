#include <string>

#include "gtest/gtest.h"

#include "core/json_utils.h"
#include "fuzztest/fuzztest.h"
#include "mcp/gateway/config.h"
#include "mcp/gateway/result.h"
#include "mcp/types.h"

namespace slop::mcp::gateway {
namespace {

void ConfigParsingIsTotal(const std::string& text) {
  if (text.size() > 1024 * 1024) return;
  const auto result = ParseConfigText(text);
  if (!result.ok()) EXPECT_FALSE(result.status().ok());
}

void ResultNormalizationIsBounded(const std::string& text, bool is_error) {
  if (text.size() > 4096) return;
  ToolCallResult result;
  result.is_error = is_error;
  result.content.push_back({{"type", "text"}, {"text", text}});
  const auto normalized = NormalizeToolResult(result, 1024);
  if (!normalized.ok()) EXPECT_FALSE(normalized.status().ok());
  if (normalized.ok() && normalized->ok) EXPECT_LE(json_dump(normalized->value).size(), 1024);
  if (normalized.ok() && !normalized->ok) EXPECT_LE(normalized->error.size(), 1024);
}

FUZZ_TEST(GatewayFuzzTest, ConfigParsingIsTotal).WithDomains(fuzztest::Arbitrary<std::string>());
FUZZ_TEST(GatewayFuzzTest, ResultNormalizationIsBounded)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<bool>());

}  // namespace
}  // namespace slop::mcp::gateway
