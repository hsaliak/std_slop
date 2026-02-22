#include "core/json_utils.h"

#include <gtest/gtest.h>

namespace slop {

TEST(JsonUtilsTest, ParseValidJson) {
  auto j = json_parse("{\"key\": \"value\"}");
  ASSERT_TRUE(j.has_value());
  EXPECT_EQ((*j)["key"], "value");
}

TEST(JsonUtilsTest, ParseInvalidJson) {
  auto j = json_parse("{\"key\": \"value\"");  // missing closing brace
  EXPECT_FALSE(j.has_value());
}

TEST(JsonUtilsTest, GetString) {
  auto j = nlohmann::json::parse("{\"key\": \"value\"}");
  auto val = json_get<std::string>(j, "key");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, "value");
}

TEST(JsonUtilsTest, GetMissingKey) {
  auto j = nlohmann::json::parse("{\"key\": \"value\"}");
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
  auto j = nlohmann::json::parse("{\"key\": [1, \"two\", 3]}");
  auto val = json_get<std::vector<int>>(j, "key");
  EXPECT_FALSE(val.has_value());
}

TEST(JsonUtilsTest, GetOr) {
  auto j = nlohmann::json::parse("{\"key\": \"value\"}");
  EXPECT_EQ(json_get_or<std::string>(j, "key", "default"), "value");
  EXPECT_EQ(json_get_or<std::string>(j, "missing", "default"), "default");
  EXPECT_EQ(json_get_or<int>(j, "key", 42), 42);  // wrong type
}

}  // namespace slop
