#include "tools/edit_engine.h"

#include "absl/status/status.h"
#include "gtest/gtest.h"

namespace slop {
namespace {

TEST(EditEngineTest, ReplacesUniqueMatchAndReportsChange) {
  const nlohmann::json edits = {{{"op", "replace"}, {"find", "beta"}, {"text", "BETA"}}};
  auto result = ApplyTextEdits("alpha beta gamma", edits);
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result->content, "alpha BETA gamma");
  EXPECT_EQ(result->edit_count, 1);
  ASSERT_EQ(result->changes.size(), 1);
  EXPECT_EQ(result->changes[0]["matches"], 1);
  EXPECT_EQ(result->changes[0]["old"], 4);
  EXPECT_EQ(result->changes[0]["new"], 4);
}

TEST(EditEngineTest, MatchSelectorSupportsFirstLastAndIndex) {
  const nlohmann::json edits = {{{"op", "replace"}, {"find", "x"}, {"text", "y"}, {"which", "last"}}};
  auto result = ApplyTextEdits("x x x", edits);
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result->content, "x x y");

  const nlohmann::json indexed = {{{"op", "delete"}, {"find", "x"}, {"which", 1}}};
  auto indexed_result = ApplyTextEdits("x x x", indexed);
  ASSERT_TRUE(indexed_result.ok()) << indexed_result.status().message();
  EXPECT_EQ(indexed_result->content, "x  x");
}

TEST(EditEngineTest, RejectsInvalidOrAmbiguousEditsWithoutChangingInput) {
  const std::string original = "x x";
  EXPECT_FALSE(ValidateTextEdits(nlohmann::json::object()).ok());
  EXPECT_FALSE(ValidateTextEdits(nlohmann::json::array()).ok());

  const nlohmann::json ambiguous = {{{"op", "delete"}, {"find", "x"}}};
  EXPECT_TRUE(ValidateTextEdits(ambiguous).ok());
  auto result = ApplyTextEdits(original, ambiguous);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(original, "x x");
}

TEST(EditEngineTest, NullSelectorUsesDefaultUniqueMatchPolicy) {
  const nlohmann::json edits = {{{"op", "delete"}, {"find", "x"}, {"which", nullptr}}};
  auto result = ApplyTextEdits("x", edits);
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_TRUE(result->content.empty());
}

TEST(EditEngineTest, RejectsExcessiveEditCount) {
  nlohmann::json edits = nlohmann::json::array();
  for (int index = 0; index < 257; ++index) {
    edits.push_back({{"op", "delete"}, {"find", "x"}});
  }
  const absl::Status status = ValidateTextEdits(edits);
  EXPECT_EQ(status.code(), absl::StatusCode::kResourceExhausted);
}

TEST(EditEngineTest, RejectsNoOpAndInvalidMatchSelector) {
  const nlohmann::json no_op = {{{"op", "replace"}, {"find", "x"}, {"text", "x"}}};
  EXPECT_FALSE(ValidateTextEdits(no_op).ok());

  const nlohmann::json bad_selector = {{{"op", "delete"}, {"find", "x"}, {"which", -1}}};
  EXPECT_FALSE(ValidateTextEdits(bad_selector).ok());
}

}  // namespace
}  // namespace slop
