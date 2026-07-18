#include "core/json_utils.h"

#include <gtest/gtest.h>

namespace slop {

TEST(JsonUtilsTest, ParseValidJson) {
  auto j = json_parse(R"({"key": "value"})");
  ASSERT_TRUE(j.has_value());
  EXPECT_EQ((*j)["key"], "value");
}

TEST(JsonUtilsTest, ParseInvalidJson) {
  auto j = json_parse(R"({"key": "value")");  // missing closing brace
  EXPECT_FALSE(j.has_value());
}

TEST(JsonUtilsTest, GetString) {
  auto j = nlohmann::json::parse(R"({"key": "value"})");
  auto val = json_get<std::string>(j, "key");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, "value");
}

TEST(JsonUtilsTest, GetMissingKey) {
  auto j = nlohmann::json::parse(R"({"key": "value"})");
  auto val = json_get<std::string>(j, "other_key");
  EXPECT_FALSE(val.has_value());
}

TEST(JsonUtilsTest, GetWrongType) {
  auto j = nlohmann::json::parse("{\"key\": 123}");
  auto val = json_get<std::string>(j, "key");
  EXPECT_FALSE(val.has_value());
}

TEST(JsonUtilsTest, GetInt) {
  auto j = nlohmann::json::parse("{\"key\": 123}");
  auto val = json_get<int>(j, "key");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, 123);
}

TEST(JsonUtilsTest, GetVector) {
  auto j = nlohmann::json::parse("{\"key\": [1, 2, 3]}");
  auto val = json_get<std::vector<int>>(j, "key");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val->size(), 3);
  EXPECT_EQ((*val)[0], 1);
  EXPECT_EQ((*val)[2], 3);
}

TEST(JsonUtilsTest, GetVectorWrongType) {
  auto j = nlohmann::json::parse(R"({"key": [1, "two", 3]})");
  auto val = json_get<std::vector<int>>(j, "key");
  EXPECT_FALSE(val.has_value());
}

TEST(JsonUtilsTest, GetOr) {
  auto j = nlohmann::json::parse(R"({"key": "value"})");
  EXPECT_EQ(json_get_or<std::string>(j, "key", "default"), "value");
  EXPECT_EQ(json_get_or<std::string>(j, "missing", "default"), "default");
  EXPECT_EQ(json_get_or<int>(j, "key", 42), 42);  // wrong type
}

TEST(JsonUtilsTest, ValidatesStructuredOutputSchemaAndValue) {
  auto schema = json_parse(R"({"type":"object","properties":{"name":{"type":"string"},"tags":{"type":"array","items":{"type":"integer"}}},"required":["name"],"additionalProperties":false})");
  ASSERT_TRUE(schema.has_value());
  EXPECT_TRUE(ValidateStructuredOutputSchema(*schema).ok());

  auto value = json_parse(R"({"name":"slop","tags":[1,2]})");
  ASSERT_TRUE(value.has_value());
  EXPECT_TRUE(ValidateJsonAgainstSchema(*value, *schema).ok());
}

TEST(JsonUtilsTest, RejectsInvalidStructuredOutputSchema) {
  auto unsupported = json_parse(R"({"type":"object","oneOf":[]})");
  ASSERT_TRUE(unsupported.has_value());
  EXPECT_FALSE(ValidateStructuredOutputSchema(*unsupported).ok());

  auto non_object_root = json_parse(R"({"type":"array","items":{"type":"string"}})");
  ASSERT_TRUE(non_object_root.has_value());
  EXPECT_FALSE(ValidateStructuredOutputSchema(*non_object_root).ok());
}

TEST(JsonUtilsTest, RejectsValueThatViolatesStructuredOutputSchema) {
  auto schema = json_parse(R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"],"additionalProperties":false})");
  ASSERT_TRUE(schema.has_value());
  auto missing_name = json_parse(R"({})");
  ASSERT_TRUE(missing_name.has_value());
  EXPECT_FALSE(ValidateJsonAgainstSchema(*missing_name, *schema).ok());

  auto unexpected_property = json_parse(R"({"name":"slop","extra":true})");
  ASSERT_TRUE(unexpected_property.has_value());
  EXPECT_FALSE(ValidateJsonAgainstSchema(*unexpected_property, *schema).ok());
}

}  // namespace slop
