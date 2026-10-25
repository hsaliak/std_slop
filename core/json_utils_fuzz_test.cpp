
#include "core/json_utils.h"

#include <cstdint>
#include <limits>
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
    return;
  }

  const std::string dumped = json_dump(*parsed);
  auto reparsed = json_parse(dumped);
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(json_dump(*parsed), json_dump(*reparsed));
}

void GetOrUsesDefaultOnlyWhenIntegerFieldIsUnavailable(const std::string& raw_json, const std::string& key,
                                                       int default_value) {
  const auto parsed = json_parse(raw_json);
  if (!parsed.has_value()) return;

  const auto value = json_get<int>(*parsed, key);
  EXPECT_EQ(json_get_or<int>(*parsed, key, default_value), value.value_or(default_value));
}

void IntegerGettersRejectOutOfRangeValues(const std::string& raw_json, const std::string& key) {
  const auto parsed = json_parse(raw_json);
  if (!parsed.has_value()) return;

  const nlohmann::json* value = json_at(*parsed, key);
  const auto int_value = json_get<int>(*parsed, key);
  const auto int64_value = json_get<std::int64_t>(*parsed, key);
  if (value == nullptr || !value->is_number_integer()) {
    EXPECT_FALSE(int_value.has_value());
    EXPECT_FALSE(int64_value.has_value());
    return;
  }

  if (value->is_number_unsigned()) {
    const std::uint64_t unsigned_value = value->get<std::uint64_t>();
    EXPECT_EQ(int_value.has_value(), unsigned_value <= static_cast<std::uint64_t>(std::numeric_limits<int>::max()));
    EXPECT_EQ(int64_value.has_value(), unsigned_value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
    if (int_value) EXPECT_EQ(*int_value, static_cast<int>(unsigned_value));
    if (int64_value) EXPECT_EQ(*int64_value, static_cast<std::int64_t>(unsigned_value));
    return;
  }

  const std::int64_t signed_value = value->get<std::int64_t>();
  const bool fits_int = signed_value >= std::numeric_limits<int>::min() &&
                        signed_value <= std::numeric_limits<int>::max();
  EXPECT_EQ(int_value.has_value(), fits_int);
  EXPECT_TRUE(int64_value.has_value());
  if (int_value) EXPECT_EQ(*int_value, static_cast<int>(signed_value));
  EXPECT_EQ(*int64_value, signed_value);
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

void StructuredSchemaValidationRejectsMalformedSchemas(const std::string& raw_schema, const std::string& raw_value) {
  const auto schema = json_parse(raw_schema);
  const auto value = json_parse(raw_value);
  if (!schema.has_value()) return;

  const absl::Status schema_status = ValidateStructuredOutputSchema(*schema);
  if (!schema_status.ok()) {
    EXPECT_FALSE(schema_status.message().empty());
    return;
  }
  EXPECT_TRUE(json_is<nlohmann::json::object_t>(*schema));
  EXPECT_EQ(json_get_or<std::string>(*schema, "type", ""), "object");
  if (!value.has_value()) return;

  const absl::Status value_status = ValidateJsonAgainstSchema(*value, *schema);
  if (!value_status.ok()) EXPECT_FALSE(value_status.message().empty());
}

FUZZ_TEST(JsonUtilsFuzzTest, ParseAndDumpNeverCrashes);

FUZZ_TEST(JsonUtilsFuzzTest, GetOrUsesDefaultOnlyWhenIntegerFieldIsUnavailable)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<int>());

FUZZ_TEST(JsonUtilsFuzzTest, IntegerGettersRejectOutOfRangeValues)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>());

FUZZ_TEST(JsonUtilsFuzzTest, ArrayGetterRequiresHomogeneousTypes)
    .WithDomains(fuzztest::VectorOf(fuzztest::Arbitrary<int>()), fuzztest::Arbitrary<std::string>());

FUZZ_TEST(JsonUtilsFuzzTest, StructuredSchemaValidationRejectsMalformedSchemas)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>());

}  // namespace
}  // namespace slop
