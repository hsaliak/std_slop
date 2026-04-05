
#include "acp/capabilities.h"

#include <string>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace slop::acp {
namespace {

void ParseInitializeParamsNoCrash(const std::string& protocol_version,
                                  const std::string& capabilities_blob,
                                  const std::string& runtime_options_blob) {
  nlohmann::json params = nlohmann::json::object();
  params["protocolVersion"] = protocol_version;

  auto caps = nlohmann::json::parse(capabilities_blob, nullptr, false);
  if (caps.is_discarded()) {
    params["capabilities"] = capabilities_blob;
  } else {
    params["capabilities"] = caps;
  }

  auto runtime = nlohmann::json::parse(runtime_options_blob, nullptr, false);
  if (runtime.is_discarded()) {
    params["runtimeOptions"] = runtime_options_blob;
  } else {
    params["runtimeOptions"] = runtime;
  }

  auto parsed_or = ParseInitializeParams(params);
  if (parsed_or.ok()) {
    EXPECT_EQ(parsed_or->protocol_version, "1");
    EXPECT_TRUE(parsed_or->client_capabilities.is_object());
    EXPECT_TRUE(parsed_or->runtime_options.is_object());
  }
}

FUZZ_TEST(InitializeFuzzTest, ParseInitializeParamsNoCrash)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>());

}  // namespace
}  // namespace slop::acp