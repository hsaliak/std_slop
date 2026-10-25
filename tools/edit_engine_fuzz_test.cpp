#include "tools/edit_engine.h"

#include <string>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop {
namespace {

void EditEngineIsDeterministicForArbitraryInput(const std::string& original, const std::string& edits_raw) {
  nlohmann::json edits = nlohmann::json::parse(edits_raw, nullptr, false);
  if (edits.is_discarded()) edits = edits_raw;

  const absl::Status validation = ValidateTextEdits(edits);
  const auto first = ApplyTextEdits(original, edits);
  const auto second = ApplyTextEdits(original, edits);
  EXPECT_EQ(first.ok(), second.ok());
  if (!first.ok()) {
    EXPECT_EQ(first.status().code(), second.status().code());
    EXPECT_EQ(first.status().message(), second.status().message());
    return;
  }

  EXPECT_TRUE(validation.ok());
  EXPECT_NE(first->content, original);
  EXPECT_EQ(first->content, second->content);
  EXPECT_EQ(first->changes, second->changes);
  EXPECT_EQ(first->edit_count, first->changes.size());
}

FUZZ_TEST(EditEngineFuzzTest, EditEngineIsDeterministicForArbitraryInput)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>());

}  // namespace
}  // namespace slop
