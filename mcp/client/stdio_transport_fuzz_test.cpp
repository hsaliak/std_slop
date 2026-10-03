#include <cstddef>
#include <string>

#include "absl/status/status.h"
#include "gtest/gtest.h"

#include "fuzztest/fuzztest.h"
#include "mcp/client/stdio_transport_internal.h"

namespace slop::mcp::stdio_internal {
namespace {

void FrameBufferInputDoesNotCrash(const std::string& input, size_t max_frame_bytes) {
  const size_t limit = 1 + max_frame_bytes % 4096;
  std::string buffer = input;
  auto frame_or = PopFrame(&buffer, limit);
  if (!frame_or.ok()) return;
  if (frame_or->has_value()) {
    EXPECT_LE(frame_or->value().size(), limit);
    EXPECT_EQ(frame_or->value().find('\n'), std::string::npos);
  } else {
    EXPECT_LE(buffer.size(), limit);
    EXPECT_EQ(buffer.find('\n'), std::string::npos);
    EXPECT_EQ(buffer, input);
  }
}
FUZZ_TEST(StdioFrameFuzzTest, FrameBufferInputDoesNotCrash);

TEST(StdioFrameFuzzTest, EnforcesExactLimitAndPreservesFollowingFrames) {
  auto null_buffer = PopFrame(nullptr, 4);
  ASSERT_FALSE(null_buffer.ok());
  EXPECT_EQ(null_buffer.status().code(), absl::StatusCode::kInvalidArgument);

  std::string partial = "1234";
  auto incomplete = PopFrame(&partial, 4);
  ASSERT_TRUE(incomplete.ok());
  EXPECT_FALSE(incomplete->has_value());
  EXPECT_EQ(partial, "1234");

  std::string buffer = "1234\nnext\n";
  auto first = PopFrame(&buffer, 4);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(first->has_value());
  EXPECT_EQ(first->value(), "1234");
  EXPECT_EQ(buffer, "next\n");

  auto second = PopFrame(&buffer, 4);
  ASSERT_TRUE(second.ok());
  ASSERT_TRUE(second->has_value());
  EXPECT_EQ(second->value(), "next");
  EXPECT_TRUE(buffer.empty());

  std::string oversized = "12345\n";
  auto rejected = PopFrame(&oversized, 4);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.status().code(), absl::StatusCode::kResourceExhausted);
}

}  // namespace
}  // namespace slop::mcp::stdio_internal
