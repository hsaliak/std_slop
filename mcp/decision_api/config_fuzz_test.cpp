#include <string>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "fuzztest/fuzztest.h"
#include "mcp/decision_api/config.h"

namespace slop::mcp::decision_api {
namespace {

void ParseConfigTextIsTotal(const std::string& text) {
  if (text.size() > 64 * 1024) return;
  auto config = ParseConfigText(text);
  if (config.ok()) {
    EXPECT_FALSE(config->api_key.empty());
    EXPECT_FALSE(config->model.empty());
    EXPECT_EQ(config->endpoint.rfind("https://", 0), 0);
  }
}

FUZZ_TEST(DecisionApiConfigFuzzTest, ParseConfigTextIsTotal).WithDomains(fuzztest::Arbitrary<std::string>());

}  // namespace
}  // namespace slop::mcp::decision_api
