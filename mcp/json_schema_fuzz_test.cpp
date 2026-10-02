#include <string>

#include "absl/status/status.h"
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

void TypeNamesCheckedBeforeValidation(const std::string& name) {
  const bool valid = name == "null" || name == "boolean" || name == "object" || name == "array" || name == "number" ||
                     name == "integer" || name == "string";
  EXPECT_EQ(CheckJsonSchema({{"type", name}}).ok(), valid);
  EXPECT_EQ(CheckJsonSchema({{"properties", {{"value", {{"type", name}}}}}}).ok(), valid);
}
FUZZ_TEST(JsonSchemaFuzzTest, TypeNamesCheckedBeforeValidation);

void CombinatorsNeverHideWorkLimits(unsigned int choice, unsigned int count) {
  const char* keywords[] = {"not", "allOf", "anyOf", "oneOf"};
  const std::string keyword = keywords[choice % 4];
  const nlohmann::json branch = {{"type", "array"}, {"items", {{"type", "integer"}}}};
  const nlohmann::json schema = {{keyword, keyword == "not" ? branch : nlohmann::json::array({true, branch})}};
  auto instance = nlohmann::json::array();
  for (unsigned int i = 0; i < 4 + count % 32; ++i) instance.push_back(1);
  JsonSchemaLimits limits;
  limits.max_evaluations = 4;
  ASSERT_TRUE(CheckJsonSchema(schema, limits).ok());
  EXPECT_EQ(ValidateJsonSchema(schema, instance, limits).code(), absl::StatusCode::kResourceExhausted);
}
FUZZ_TEST(JsonSchemaFuzzTest, CombinatorsNeverHideWorkLimits);

void EmptySchemaArraysNeverRegister(unsigned int choice, bool nested) {
  const char* keywords[] = {"allOf", "anyOf", "oneOf", "prefixItems"};
  nlohmann::json schema = {{keywords[choice % 4], nlohmann::json::array()}};
  if (nested) schema = {{"properties", {{"value", schema}}}};
  EXPECT_EQ(CheckJsonSchema(schema).code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(ValidateJsonSchema(schema, nlohmann::json::object()).code(), absl::StatusCode::kInvalidArgument);
}
FUZZ_TEST(JsonSchemaFuzzTest, EmptySchemaArraysNeverRegister);

void StringLengthsUseCodePoints(unsigned int choice, unsigned int count) {
  const std::string characters[] = {"a", "\xc3\xa9", "\xe2\x98\x83", "\xf0\x9f\x98\x80", std::string(1, '\0')};
  const unsigned int length = count % 33;
  std::string text;
  for (unsigned int i = 0; i < length; ++i) text += characters[choice % 5];
  EXPECT_TRUE(ValidateJsonSchema({{"minLength", length}, {"maxLength", length}}, text).ok());
  EXPECT_EQ(ValidateJsonSchema({{"minLength", length + 1}}, text).code(), absl::StatusCode::kInvalidArgument);
  if (length > 0) {
    EXPECT_EQ(ValidateJsonSchema({{"maxLength", length - 1}}, text).code(), absl::StatusCode::kInvalidArgument);
  }
}
FUZZ_TEST(JsonSchemaFuzzTest, StringLengthsUseCodePoints);

void MalformedUtf8NeverValidates(const std::string& tail, unsigned int choice) {
  const std::string prefixes[] = {"\x80", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80"};
  const std::string text = prefixes[choice % 4] + tail.substr(0, 256);
  EXPECT_EQ(ValidateJsonSchema({{"type", "string"}}, text).code(), absl::StatusCode::kInvalidArgument);
}
FUZZ_TEST(JsonSchemaFuzzTest, MalformedUtf8NeverValidates);

TEST(JsonSchemaFuzzTest, RegressionSeeds) {
  JsonSchemaNeverCrashes(R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object"})",
                         R"({"name":"tool"})");
  JsonSchemaNeverCrashes(R"({"$ref":"#/$defs/node","$defs":{"node":{"$ref":"#/$defs/node"}}})", "null");
  JsonSchemaNeverCrashes(R"({"oneOf":[false,{"type":"integer"}]})", "4");
  JsonSchemaNeverCrashes(R"({"type":"invalid","required":false})", "{}");
  TypeNamesCheckedBeforeValidation("invalid");
  TypeNamesCheckedBeforeValidation("object");
  for (unsigned int choice = 0; choice < 4; ++choice) {
    CombinatorsNeverHideWorkLimits(choice, 0);
    EmptySchemaArraysNeverRegister(choice, false);
    EmptySchemaArraysNeverRegister(choice, true);
    MalformedUtf8NeverValidates("tail", choice);
  }
  for (unsigned int choice = 0; choice < 5; ++choice) {
    StringLengthsUseCodePoints(choice, 0);
    StringLengthsUseCodePoints(choice, 3);
  }
}

}  // namespace
}  // namespace slop::mcp
