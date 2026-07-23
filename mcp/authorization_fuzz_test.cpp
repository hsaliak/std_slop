#include "mcp/authorization.h"

#include <string>
#include <tuple>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace slop::mcp {
namespace {

void WwwAuthenticateParserNeverCrashes(const std::string& header) {
  const auto parsed = ParseWwwAuthenticateResourceMetadata(header);
  if (parsed.ok()) {
    EXPECT_FALSE(parsed->empty());
  }
}

void MetadataParsersNeverCrash(const std::string& raw) {
  const nlohmann::json not_object = nlohmann::json::array({raw, nullptr});
  (void)ParseProtectedResourceMetadata(not_object);
  (void)ParseAuthorizationServerMetadata(not_object);

  nlohmann::json object = nlohmann::json::object();
  object["resource"] = raw;
  object["authorization_servers"] = nlohmann::json::array({raw});
  object["scopes_supported"] = nlohmann::json::array({raw});
  object["issuer"] = raw;
  object["authorization_endpoint"] = raw;
  object["token_endpoint"] = raw;
  (void)ParseProtectedResourceMetadata(object);
  (void)ParseAuthorizationServerMetadata(object);
}

FUZZ_TEST(McpAuthorizationFuzzTest, WwwAuthenticateParserNeverCrashes)
    .WithSeeds(std::vector<std::tuple<std::string>>{
        std::make_tuple(std::string(R"(Bearer resource_metadata="https://example.com/meta")")),
        std::make_tuple(std::string("Bearer realm=example")),
        std::make_tuple(std::string("Basic realm=example")),
        std::make_tuple(std::string("Bearer ")),
    });

FUZZ_TEST(McpAuthorizationFuzzTest, MetadataParsersNeverCrash)
    .WithSeeds(std::vector<std::tuple<std::string>>{
        std::make_tuple(std::string("https://example.com")),
        std::make_tuple(std::string("")),
        std::make_tuple(std::string("read write")),
    });

}  // namespace
}  // namespace slop::mcp
