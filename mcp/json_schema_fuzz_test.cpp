#include <string>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "fuzztest/fuzztest.h"
#include "mcp/json_schema.h"

namespace slop::mcp {
namespace {

void JsonSchemaNeverCrashes(const std::string& schema_text, const std::string& instance_text) {
  const nlohmann::json schema = nlohmann::json::parse(schema_text, nullptr, false);
  const nlohmann::json instance = nlohmann::json::parse(instance_text, nullptr, false);
  if (schema.is_discarded() || instance.is_discarded()) return;
  JsonSchemaLimits limits;
  limits.max_evaluations = 256;
  limits.max_depth = 24;
  (void)CheckJsonSchema(schema, limits);
  (void)ValidateJsonSchema(schema, instance, limits);
}
FUZZ_TEST(JsonSchemaFuzzTest, JsonSchemaNeverCrashes);

TEST(JsonSchemaFuzzTest, RegressionSeeds) {
  JsonSchemaNeverCrashes(R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object"})",
                         R"({"name":"tool"})");
  JsonSchemaNeverCrashes(R"({"$ref":"#/$defs/node","$defs":{"node":{"$ref":"#/$defs/node"}}})", "null");
  JsonSchemaNeverCrashes(R"({"oneOf":[false,{"type":"integer"}]})", "4");
}

}  // namespace
}  // namespace slop::mcp
