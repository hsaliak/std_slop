
#include "app/mode_validation.h"

#include <gtest/gtest.h>

namespace slop {
namespace {

TEST(ModeValidationTest, AcpAndPromptConflict) {
  auto status = ValidateModeFlags(true, "hello");
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
}

TEST(ModeValidationTest, AcpWithoutPromptOk) {
  auto status = ValidateModeFlags(true, "");
  EXPECT_TRUE(status.ok());
}

TEST(ModeValidationTest, PromptWithoutAcpOk) {
  auto status = ValidateModeFlags(false, "hello");
  EXPECT_TRUE(status.ok());
}

TEST(ModeValidationTest, InteractiveDefaultOk) {
  auto status = ValidateModeFlags(false, "");
  EXPECT_TRUE(status.ok());
}

}  // namespace
}  // namespace slop