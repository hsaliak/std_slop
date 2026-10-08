#include <string>

#include "gtest/gtest.h"

#include "core/json_utils.h"
#include "fuzztest/fuzztest.h"
#include "mcp/decision_api/client.h"

namespace slop::mcp::decision_api {
namespace {

void DecisionRequestAndResponseParsingIsTotal(const std::string& request_text, const std::string& response_text) {
  if (request_text.size() > 64 * 1024 || response_text.size() > 64 * 1024) return;
  auto request = json_parse(request_text);
  auto response = json_parse(response_text);
  if (!request.has_value() || !response.has_value()) return;
  Config config;
  auto built = DecisionApiClient::BuildRequest(*request, config);
  if (built.ok()) (void)DecisionApiClient::ValidateResponse(*built, *response);
}

FUZZ_TEST(DecisionApiClientFuzzTest, DecisionRequestAndResponseParsingIsTotal)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>());

}  // namespace
}  // namespace slop::mcp::decision_api
