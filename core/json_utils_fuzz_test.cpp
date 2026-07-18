
#include "core/json_utils.h"

#include <string>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace slop {
namespace {

void ParseAndDumpNeverCrashes(const std::string& input) {
  auto parsed = json_parse(input);
  if (!parsed.has_value()) {
    EXPECT_FALSE(parsed.has_value());
    return;
  }

  const std::string dumped = json_dump(*parsed);
  auto reparsed = json_parse(dumped);
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(json_dump(*parsed), json_dump(*reparsed));
}

void GetOrIsDeterministicForTypeMismatch(const std::string& raw_json, const std::string& key, int default_value) {
  auto parsed = json_parse(raw_json);
  if (!parsed.has_value()) {
    return;
  }

  const int first = json_get_or<int>(*parsed, key, default_value);
  const int second = json_get_or<int>(*parsed, key, default_value);
  EXPECT_EQ(first, second);
}

void ArrayGetterRequiresHomogeneousTypes(const std::vector<int>& values, const std::string& bad_tail) {
  nlohmann::json j;
  j["ok"] = values;
  nlohmann::json mixed = nlohmann::json::array();
  for (int v : values) {
    mixed.push_back(v);
  }
  mixed.push_back(bad_tail);
  j["mixed"] = mixed;

  auto ok = json_get<std::vector<int>>(j, "ok");
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(*ok, values);

  auto bad = json_get<std::vector<int>>(j, "mixed");
  EXPECT_FALSE(bad.has_value());
}

void StructuredSchemaValidationNeverCrashes(const std::string& raw_schema, const std::string& raw_value) {
  const auto schema = json_parse(raw_schema);
  const auto value = json_parse(raw_value);
  if (!schema.has_value()) return;

  const absl::Status first_schema_status = ValidateStructuredOutputSchema(*schema);
  const absl::Status second_schema_status = ValidateStructuredOutputSchema(*schema);
  EXPECT_EQ(first_schema_status, second_schema_status);
  if (!first_schema_status.ok() || !value.has_value()) return;

  const absl::Status first_value_status = ValidateJsonAgainstSchema(*value, *schema);
  const absl::Status second_value_status = ValidateJsonAgainstSchema(*value, *schema);
  EXPECT_EQ(first_value_status, second_value_status);
}

FUZZ_TEST(JsonUtilsFuzzTest, ParseAndDumpNeverCrashes);

FUZZ_TEST(JsonUtilsFuzzTest, GetOrIsDeterministicForTypeMismatch)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<int>());

FUZZ_TEST(JsonUtilsFuzzTest, ArrayGetterRequiresHomogeneousTypes)
    .WithDomains(fuzztest::VectorOf(fuzztest::Arbitrary<int>()), fuzztest::Arbitrary<std::string>());

FUZZ_TEST(JsonUtilsFuzzTest, StructuredSchemaValidationNeverCrashes)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>());

}  // namespace
}  // namespace slop
