#include "core/responses_event_decoder.h"

#include <gtest/gtest.h>

namespace slop {

TEST(ResponsesEventDecoderTest, DecodesTextDeltaAcrossChunks) {
  ResponsesEventDecoder decoder;
  auto first = decoder.Feed("event: response.output_text.delta\ndata: {\"type\":\"response.output_");
  ASSERT_TRUE(first.ok());
  EXPECT_TRUE(first->empty());
  auto second = decoder.Feed("text.delta\",\"delta\":\"Hello\"}\n\n");
  ASSERT_TRUE(second.ok());
  ASSERT_EQ(second->size(), 1);
  EXPECT_EQ((*second)[0].type, ResponsesEventType::kTextDelta);
  EXPECT_EQ((*second)[0].text, "Hello");
}

TEST(ResponsesEventDecoderTest, FinishFlushesFinalEvent) {
  ResponsesEventDecoder decoder;
  ASSERT_TRUE(decoder.Feed("data: {\"type\":\"response.output_text.done\",\"text\":\"Done\"}").ok());
  auto events = decoder.Finish();
  ASSERT_TRUE(events.ok());
  ASSERT_EQ(events->size(), 1);
  EXPECT_EQ((*events)[0].type, ResponsesEventType::kTextDone);
  EXPECT_EQ((*events)[0].text, "Done");
}

TEST(ResponsesEventDecoderTest, DecodesMultipleEventsInOneChunk) {
  ResponsesEventDecoder decoder;
  auto events = decoder.Feed(
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"A\"}\n\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\"}}\n\n");
  ASSERT_TRUE(events.ok());
  ASSERT_EQ(events->size(), 2);
  EXPECT_EQ((*events)[0].type, ResponsesEventType::kTextDelta);
  EXPECT_EQ((*events)[1].type, ResponsesEventType::kCompleted);
}

TEST(ResponsesEventDecoderTest, NormalizationDeduplicatesCompletedFunctionCall) {
  const std::string payload =
      "data: {\"type\":\"response.output_item.added\",\"item\":{\"type\":\"function_call\",\"call_id\":\"call_1\",\"name\":\"query_db\",\"arguments\":\"\"}}\n\n"
      "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"function_call\",\"call_id\":\"call_1\",\"name\":\"query_db\",\"arguments\":\"{}\"}}\n\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"output\":[{\"type\":\"function_call\",\"call_id\":\"call_1\",\"name\":\"query_db\",\"arguments\":\"{}\",\"provider_metadata\":{\"index\":1}}]}}\n\n";
  auto normalized = ResponsesEventDecoder::NormalizeSsePayload(payload);
  ASSERT_TRUE(normalized.has_value());
  ASSERT_EQ((*normalized)["output"].size(), 1);
  EXPECT_EQ((*normalized)["output"][0]["arguments"], "{}");
  EXPECT_EQ((*normalized)["output"][0]["provider_metadata"]["index"], 1);
}

TEST(ResponsesEventDecoderTest, NormalizationHandlesNonObjectError) {
  auto normalized = ResponsesEventDecoder::NormalizeSsePayload("data: {\"type\":\"response.failed\",\"error\":\"bad\"}\n\n");
  ASSERT_TRUE(normalized.has_value());
  EXPECT_EQ((*normalized)["status"], "failed");
  EXPECT_EQ((*normalized)["error"]["message"], "");
}

TEST(ResponsesEventDecoderTest, NormalizationPreservesNestedResponseErrorObject) {
  auto normalized = ResponsesEventDecoder::NormalizeSsePayload(
      "data: {\"type\":\"response.failed\",\"response\":{\"status\":\"failed\",\"error\":{"
      "\"code\":\"server_error\",\"message\":\"Our servers are currently overloaded\"}}}\n\n");
  ASSERT_TRUE(normalized.has_value());
  EXPECT_EQ((*normalized)["status"], "failed");
  EXPECT_EQ((*normalized)["error"]["code"], "server_error");
  EXPECT_EQ((*normalized)["error"]["message"], "Our servers are currently overloaded");
}

TEST(ResponsesEventDecoderTest, NormalizationPreservesErrorEventPayload) {
  auto normalized = ResponsesEventDecoder::NormalizeSsePayload(
      "data: {\"type\":\"error\",\"code\":\"server_is_overloaded\","
      "\"message\":\"Our servers are currently overloaded\",\"param\":null,\"sequence_number\":1}\n\n");
  ASSERT_TRUE(normalized.has_value());
  EXPECT_EQ((*normalized)["status"], "failed");
  EXPECT_EQ((*normalized)["error"]["type"], "error");
  EXPECT_EQ((*normalized)["error"]["code"], "server_is_overloaded");
  EXPECT_EQ((*normalized)["error"]["message"], "Our servers are currently overloaded");
  EXPECT_TRUE((*normalized)["error"]["param"].is_null());
}

TEST(ResponsesEventDecoderTest, RejectsMalformedEvent) {
  ResponsesEventDecoder decoder;
  auto events = decoder.Feed("data: not-json\n\n");
  ASSERT_FALSE(events.ok());
  EXPECT_EQ(events.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(ResponsesEventDecoderTest, RejectsFeedAfterFinish) {
  ResponsesEventDecoder decoder;
  ASSERT_TRUE(decoder.Finish().ok());
  EXPECT_FALSE(decoder.Feed("data: {}\n\n").ok());
}

}  // namespace slop
