#include "tools/edit_engine.h"

#include <string>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop {
namespace {

void EditEngineRejectsMalformedEditsAndReturnsConsistentSummaries(const std::string& original,
                                                                   const std::string& edits_raw) {
  nlohmann::json edits = nlohmann::json::parse(edits_raw, nullptr, false);
  if (edits.is_discarded()) edits = edits_raw;

  const absl::Status validation = ValidateTextEdits(edits);
  const auto result = ApplyTextEdits(original, edits);
  if (!validation.ok()) {
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), validation.code());
    EXPECT_EQ(result.status().message(), validation.message());
    return;
  }

  if (!result.ok()) {
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_FALSE(result.status().message().empty());
    return;
  }
  EXPECT_NE(result->content, original);
  EXPECT_EQ(result->edit_count, result->changes.size());
}

FUZZ_TEST(EditEngineFuzzTest, EditEngineRejectsMalformedEditsAndReturnsConsistentSummaries)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>());

}  // namespace
}  // namespace slop
