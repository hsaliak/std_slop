#include "app/llm_tool_specializations.h"

#include <string>
#include <tuple>
#include <vector>

#include "core/json_utils.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop {
namespace {

void QueryArgumentValidationIsDeterministic(const std::string& raw) {
  auto parsed = json_parse(raw);
  const nlohmann::json args = parsed ? *parsed : nlohmann::json(raw);
  const auto expected = json_get<std::string>(args, "query");
  const auto first = ParseLlmQueryArgument(args);
  const auto second = ParseLlmQueryArgument(args);
  ASSERT_EQ(first.ok(), expected.has_value());
  ASSERT_EQ(first.ok(), second.ok());
  if (first.ok()) {
    EXPECT_EQ(*first, *expected);
    EXPECT_EQ(*first, *second);
  } else {
    EXPECT_EQ(first.status().code(), second.status().code());
    EXPECT_EQ(first.status().message(), second.status().message());
  }
}

FUZZ_TEST(LlmQueryArgsFuzzTest, QueryArgumentValidationIsDeterministic)
    .WithSeeds(std::vector<std::tuple<std::string>>{
        std::make_tuple(std::string(R"({"query":"hello"})")),
        std::make_tuple(std::string(R"({"query":7})")),
        std::make_tuple(std::string("[]")),
        std::make_tuple(std::string("{")),
    });

}  // namespace
}  // namespace slop
