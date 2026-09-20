#include "mcp/json_schema.h"

#include "absl/status/status.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace slop::mcp {
namespace {

TEST(JsonSchemaTest, ValidatesRepresentativeToolArguments) {
  const nlohmann::json schema = {
      {"$schema", "https://json-schema.org/draft/2020-12/schema"},
      {"type", "object"},
      {"required", {"name", "count"}},
      {"properties",
       {{"name", {{"type", "string"}, {"minLength", 1}}},
        {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5}}},
        {"labels", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
      {"additionalProperties", false},
  };
  EXPECT_TRUE(ValidateJsonSchema(schema, {{"name", "sample"}, {"count", 2}, {"labels", {"a", "b"}}}).ok());
  EXPECT_EQ(ValidateJsonSchema(schema, {{"name", "sample"}, {"count", 0}}).code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(ValidateJsonSchema(schema, {{"name", "sample"}, {"count", 2}, {"extra", true}}).code(),
            absl::StatusCode::kInvalidArgument);
}

TEST(JsonSchemaTest, SupportsLocalReferencesAndComposition) {
  const nlohmann::json schema = {
      {"$defs", {{"identifier", {{"type", {"integer", "string"}}}}}},
      {"allOf", {{{"$ref", "#/$defs/identifier"}}, {{"not", {{"const", "reserved"}}}}}},
  };
  EXPECT_TRUE(ValidateJsonSchema(schema, 7).ok());
  EXPECT_TRUE(ValidateJsonSchema(schema, "ok").ok());
  EXPECT_FALSE(ValidateJsonSchema(schema, "reserved").ok());
  EXPECT_FALSE(ValidateJsonSchema(schema, false).ok());
}

TEST(JsonSchemaTest, AcceptsRecursiveSchemaWithBoundedInstance) {
  const nlohmann::json schema = {
      {"$defs", {{"node", {{"type", "object"}, {"properties", {{"next", {{"$ref", "#/$defs/node"}}}}}}}}},
      {"$ref", "#/$defs/node"},
  };
  EXPECT_TRUE(ValidateJsonSchema(schema, {{"next", {{"next", nlohmann::json::object()}}}}).ok());
}

TEST(JsonSchemaTest, RejectsUnsupportedDialectAndExternalReference) {
  EXPECT_EQ(CheckJsonSchema({{"$schema", "http://json-schema.org/draft-07/schema#"}}).code(),
            absl::StatusCode::kUnimplemented);
  EXPECT_EQ(CheckJsonSchema({{"$ref", "https://example.test/schema"}}).code(), absl::StatusCode::kUnimplemented);
  EXPECT_EQ(CheckJsonSchema({{"$ref", "#/$defs/missing"}}).code(), absl::StatusCode::kInvalidArgument);
}

TEST(JsonSchemaTest, RejectsUnsupportedKeywordsAndMalformedShapes) {
  EXPECT_EQ(CheckJsonSchema({{"pattern", "^a"}}).code(), absl::StatusCode::kUnimplemented);
  EXPECT_EQ(CheckJsonSchema({{"properties", nlohmann::json::array()}}).code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(CheckJsonSchema({{"allOf", nlohmann::json::object()}}).code(), absl::StatusCode::kInvalidArgument);
}

TEST(JsonSchemaTest, EnforcesWorkAndDepthLimits) {
  const nlohmann::json schema = {
      {"allOf", {{{"type", "integer"}}, {{"minimum", 0}}, {{"maximum", 10}}}},
  };
  JsonSchemaLimits work_limits;
  work_limits.max_evaluations = 2;
  EXPECT_EQ(ValidateJsonSchema(schema, 3, work_limits).code(), absl::StatusCode::kResourceExhausted);

  const nlohmann::json recursive = {
      {"type", "array"},
      {"items", {{"$ref", "#"}}},
  };
  JsonSchemaLimits depth_limits;
  depth_limits.max_depth = 1;
  EXPECT_EQ(ValidateJsonSchema(recursive, nlohmann::json::array({nlohmann::json::array({nlohmann::json::array()})}),
                               depth_limits)
                .code(),
            absl::StatusCode::kResourceExhausted);
}

}  // namespace
}  // namespace slop::mcp
