#include "core/responses_event_decoder.h"

#include <string>
#include <tuple>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop {
namespace {

void DecodeChunksNeverCrashes(const std::string& first, const std::string& second) {
  ResponsesEventDecoder decoder;
  auto first_events = decoder.Feed(first);
  if (!first_events.ok()) return;
  auto second_events = decoder.Feed(second);
  if (!second_events.ok()) return;
  auto final_events = decoder.Finish();
  if (!final_events.ok()) return;
  for (const auto& event : *first_events) {
    EXPECT_TRUE(event.payload.is_object());
  }
  for (const auto& event : *second_events) {
    EXPECT_TRUE(event.payload.is_object());
  }
  for (const auto& event : *final_events) {
    EXPECT_TRUE(event.payload.is_object());
  }
}

FUZZ_TEST(ResponsesEventDecoderFuzzTest, DecodeChunksNeverCrashes)
    .WithSeeds(std::vector<std::tuple<std::string, std::string>>{
        std::make_tuple("data: {\"type\":\"response.output_",
                        "text.delta\",\"delta\":\"Hello\"}\n\n"),
        std::make_tuple("data: not-json\n", "\n"),
        std::make_tuple("data: {\"type\":\"response.failed\",\"error\":\"bad\"}\n", "\n"),
    });

}  // namespace
}  // namespace slop
