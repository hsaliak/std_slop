
#include "acp/session_store.h"

#include <string>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop::acp {
namespace {

void ParseSessionNewParamsNoCrash(const std::string& session_id,
                                  bool include_session_id,
                                  bool params_object) {
  nlohmann::json params;
  if (params_object) {
    params = nlohmann::json::object();
    if (include_session_id) {
      params["sessionId"] = session_id;
    }
  } else {
    params = nlohmann::json::array();
    if (include_session_id) {
      params.push_back(session_id);
    }
  }

  auto parsed_or = ParseSessionNewParams(params);
  if (parsed_or.ok() && parsed_or->session_id.has_value()) {
    EXPECT_TRUE(IsValidSessionId(*parsed_or->session_id));
  }
}

FUZZ_TEST(SessionStoreFuzzTest, ParseSessionNewParamsNoCrash)
    .WithDomains(fuzztest::Arbitrary<std::string>(),
                 fuzztest::Arbitrary<bool>(),
                 fuzztest::Arbitrary<bool>());

}  // namespace
}  // namespace slop::acp