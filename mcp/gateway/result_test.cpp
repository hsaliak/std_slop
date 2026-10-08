#include "mcp/gateway/result.h"

#include <cstddef>
#include <string>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace slop::mcp::gateway {
namespace {

TEST(ResultTest, BoundsCompletionErrorsAndPreservesTheirPrefix) {
  for (const std::size_t size : {0, 1, 4095, 4096, 4097, 8192}) {
    const std::string error(size, 'x');
    EXPECT_EQ(BoundCompletionError(error), error.substr(0, 4096));
  }
  EXPECT_EQ(BoundCompletionError("permission denied"), "permission denied");
  const std::string binary_error("a\0b", 3);
  EXPECT_EQ(BoundCompletionError(binary_error), binary_error);
}

TEST(ResultTest, NormalizesSuccessfulResultShape) {
  ToolCallResult result;
  result.content = {{{"type", "text"}, {"text", "hello"}}};
  result.structured_content = {{"ok", true}};
  result.meta = {{"trace", "x"}};
  auto normalized = NormalizeToolResult(result);
  ASSERT_TRUE(normalized.ok()) << normalized.status();
  ASSERT_TRUE(normalized->ok);
  EXPECT_EQ(normalized->value["content"], result.content);
  EXPECT_EQ(normalized->value["structuredContent"], *result.structured_content);
  EXPECT_EQ(normalized->value["isError"], false);
  EXPECT_EQ(normalized->value["meta"], result.meta);
}

TEST(ResultTest, ConvertsToolErrorIntoCatchableError) {
  ToolCallResult result;
  result.is_error = true;
  result.content = {{{"type", "text"}, {"text", "permission denied"}}};
  auto normalized = NormalizeToolResult(result);
  ASSERT_TRUE(normalized.ok()) << normalized.status();
  EXPECT_FALSE(normalized->ok);
  EXPECT_EQ(normalized->error_category, "tool");
  EXPECT_EQ(normalized->error, "permission denied");
}

TEST(ResultTest, RejectsContinuationRatherThanReplaying) {
  ToolCallResult result;
  result.kind = ToolResultKind::kInputRequired;
  result.request_state = {{"token", "unused"}};
  auto normalized = NormalizeToolResult(result);
  ASSERT_TRUE(normalized.ok()) << normalized.status();
  EXPECT_FALSE(normalized->ok);
  EXPECT_EQ(normalized->error_category, "continuation");
  EXPECT_NE(normalized->error.find("unsupported"), std::string::npos);
}

TEST(ResultTest, BoundsSuccessAndErrorPayloads) {
  ToolCallResult success;
  success.content.push_back({{"type", "text"}, {"text", std::string(256, 'x')}});
  EXPECT_FALSE(NormalizeToolResult(success, 16).ok());

  ToolCallResult error;
  error.is_error = true;
  error.content.push_back({{"type", "text"}, {"text", std::string(256, 'x')}});
  auto normalized = NormalizeToolResult(error, 16);
  ASSERT_TRUE(normalized.ok()) << normalized.status();
  EXPECT_LE(normalized->error.size(), 16);
}

}  // namespace
}  // namespace slop::mcp::gateway
