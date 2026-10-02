#include "mcp/json_schema.h"

#include <string>

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

TEST(JsonSchemaTest, ChecksKeywordShapesWithoutAnInstance) {
  for (const auto& schema :
       {nlohmann::json{{"type", "invalid"}}, nlohmann::json{{"type", 4}},
        nlohmann::json{{"type", nlohmann::json::array()}}, nlohmann::json{{"type", {"string", "string"}}},
        nlohmann::json{{"type", {"string", "invalid"}}}, nlohmann::json{{"required", "name"}},
        nlohmann::json{{"required", {"name", 4}}}, nlohmann::json{{"required", {"name", "name"}}},
        nlohmann::json{{"enum", nlohmann::json::array()}}, nlohmann::json{{"enum", 4}},
        nlohmann::json{{"minLength", -1}}, nlohmann::json{{"maxItems", "4"}}, nlohmann::json{{"minimum", "1"}},
        nlohmann::json{{"$schema", false}}, nlohmann::json{{"properties", {{"value", {{"type", "invalid"}}}}}},
        nlohmann::json{{"$defs", {{"value", {{"type", 4}}}}}, {"$ref", "#/$defs/value"}}}) {
    EXPECT_EQ(CheckJsonSchema(schema).code(), absl::StatusCode::kInvalidArgument) << schema;
    EXPECT_EQ(ValidateJsonSchema(schema, nullptr).code(), absl::StatusCode::kInvalidArgument) << schema;
  }
  EXPECT_TRUE(CheckJsonSchema({{"type", {"string", "null"}}, {"required", nlohmann::json::array()}}).ok());
  EXPECT_TRUE(CheckJsonSchema({{"type", "integer"}, {"minimum", -4}, {"exclusiveMaximum", 5.5}}).ok());
  EXPECT_TRUE(CheckJsonSchema({{"type", "array"}, {"minItems", 0}, {"maxItems", 4}}).ok());
}

TEST(JsonSchemaTest, CombinatorsPreserveEvaluationFailures) {
  const nlohmann::json branch = {{"type", "array"}, {"items", {{"type", "integer"}}}};
  const nlohmann::json instance = nlohmann::json::array({1, 2, 3, 4});
  JsonSchemaLimits limits;
  limits.max_evaluations = 4;
  for (const std::string keyword : {"not", "allOf", "anyOf", "oneOf"}) {
    const nlohmann::json schema = {{keyword, keyword == "not" ? branch : nlohmann::json::array({true, branch})}};
    ASSERT_TRUE(CheckJsonSchema(schema, limits).ok()) << keyword;
    EXPECT_EQ(ValidateJsonSchema(schema, instance, limits).code(), absl::StatusCode::kResourceExhausted) << keyword;
  }
}

TEST(JsonSchemaTest, RejectsEmptySchemaArraysIncludingNestedSchemas) {
  for (const std::string keyword : {"allOf", "anyOf", "oneOf", "prefixItems"}) {
    const nlohmann::json empty = {{keyword, nlohmann::json::array()}};
    for (const auto& schema : {empty, nlohmann::json{{"properties", {{"value", empty}}}}}) {
      EXPECT_EQ(CheckJsonSchema(schema).code(), absl::StatusCode::kInvalidArgument) << schema;
      EXPECT_EQ(ValidateJsonSchema(schema, nlohmann::json::object()).code(), absl::StatusCode::kInvalidArgument)
          << schema;
    }
    EXPECT_TRUE(CheckJsonSchema({{keyword, nlohmann::json::array({true})}}).ok()) << keyword;
  }
}

TEST(JsonSchemaTest, StringLimitsCountUnicodeCodePoints) {
  const nlohmann::json one_character = {{"type", "string"}, {"minLength", 1}, {"maxLength", 1}};
  for (const std::string text :
       {"a", "\x7f", "\xc2\x80", "\xc3\xa9", "\xdf\xbf", "\xe0\xa0\x80", "\xe2\x98\x83", "\xed\x9f\xbf", "\xee\x80\x80",
        "\xef\xbf\xbf", "\xf0\x90\x80\x80", "\xf0\x9f\x98\x80", "\xf4\x8f\xbf\xbf"}) {
    EXPECT_TRUE(ValidateJsonSchema(one_character, text).ok());
    EXPECT_EQ(ValidateJsonSchema({{"minLength", 2}}, text).code(), absl::StatusCode::kInvalidArgument);
  }
  EXPECT_TRUE(ValidateJsonSchema(one_character, std::string(1, '\0')).ok());
  EXPECT_TRUE(ValidateJsonSchema({{"minLength", 2}, {"maxLength", 2}}, "e\xcc\x81").ok());
  EXPECT_EQ(ValidateJsonSchema(one_character, "e\xcc\x81").code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(ValidateJsonSchema({{"maxLength", 0}}, "").ok());
  EXPECT_EQ(ValidateJsonSchema(one_character, "").code(), absl::StatusCode::kInvalidArgument);
}

TEST(JsonSchemaTest, RejectsMalformedUtf8Strings) {
  for (const std::string text : {"\x80", "\xff", "\xc0\xaf", "\xc2", "\xc2!", "\xe2\x98", "\xe2(\xa1", "\xe0\x80\xaf",
                                 "\xed\xa0\x80", "\xf0\x80\x80\xaf", "\xf4\x90\x80\x80"}) {
    EXPECT_EQ(ValidateJsonSchema({{"type", "string"}}, text).code(), absl::StatusCode::kInvalidArgument);
  }
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
