#include "mcp/sse_decoder.h"

#include "absl/status/status.h"
#include "gtest/gtest.h"

namespace slop::mcp {
namespace {

TEST(SseDecoderTest, DecodesSingleEvent) {
  SseDecoder decoder;
  auto events = decoder.Feed("event: message\nid: 1\ndata: hello\n\n");

  ASSERT_TRUE(events.ok()) << events.status();
  ASSERT_EQ(events->size(), 1);
  EXPECT_EQ((*events)[0].event, "message");
  EXPECT_EQ((*events)[0].id, "1");
  EXPECT_EQ((*events)[0].data, "hello");
}

TEST(SseDecoderTest, JoinsMultilineData) {
  SseDecoder decoder;
  auto events = decoder.Feed("data: hello\ndata: world\n\n");

  ASSERT_TRUE(events.ok()) << events.status();
  ASSERT_EQ(events->size(), 1);
  EXPECT_EQ((*events)[0].data, "hello\nworld");
}

TEST(SseDecoderTest, HandlesChunkBoundaries) {
  SseDecoder decoder;
  auto first = decoder.Feed("data: hel");
  ASSERT_TRUE(first.ok()) << first.status();
  EXPECT_TRUE(first->empty());

  auto second = decoder.Feed("lo\n\n");
  ASSERT_TRUE(second.ok()) << second.status();
  ASSERT_EQ(second->size(), 1);
  EXPECT_EQ((*second)[0].data, "hello");
}

TEST(SseDecoderTest, HandlesCrLf) {
  SseDecoder decoder;
  auto events = decoder.Feed("data: hello\r\n\r\n");

  ASSERT_TRUE(events.ok()) << events.status();
  ASSERT_EQ(events->size(), 1);
  EXPECT_EQ((*events)[0].data, "hello");
}

TEST(SseDecoderTest, FinishFlushesFinalLine) {
  SseDecoder decoder;
  auto first = decoder.Feed("data: final");
  ASSERT_TRUE(first.ok()) << first.status();
  EXPECT_TRUE(first->empty());

  auto final = decoder.Finish();
  ASSERT_TRUE(final.ok()) << final.status();
  ASSERT_EQ(final->size(), 1);
  EXPECT_EQ((*final)[0].data, "final");
}

TEST(SseDecoderTest, RejectsFeedAfterFinish) {
  SseDecoder decoder;
  ASSERT_TRUE(decoder.Finish().ok());
  auto events = decoder.Feed("data: nope\n\n");

  ASSERT_FALSE(events.ok());
  EXPECT_EQ(events.status().code(), absl::StatusCode::kFailedPrecondition);
}

}  // namespace
}  // namespace slop::mcp
