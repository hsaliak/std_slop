#include "app/prompt_mode.h"

#include <fstream>
#include <sstream>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "core/json_utils.h"
#include "gtest/gtest.h"

namespace slop {
namespace {

TEST(PromptModeTest, HasPromptInputSourceDetectsAnySource) {
  EXPECT_FALSE(HasPromptInputSource({}));
  EXPECT_TRUE(HasPromptInputSource({.prompt = "hello"}));
  EXPECT_TRUE(HasPromptInputSource({.prompt_file = "task.md"}));
}

TEST(PromptModeTest, ResolvePromptInputUsesLiteralInstruction) {
  std::istringstream stdin_stream("ignored");
  auto prompt = ResolvePromptInput({.prompt = "hello"}, &stdin_stream);
  ASSERT_TRUE(prompt.ok()) << prompt.status();
  EXPECT_EQ(*prompt, "Context:\nignored\nInstruction:\nhello");
}

TEST(PromptModeTest, ResolvePromptInputUsesLiteralInstructionWithoutContextStream) {
  auto prompt = ResolvePromptInput({.prompt = "hello"}, nullptr);
  ASSERT_TRUE(prompt.ok()) << prompt.status();
  EXPECT_EQ(*prompt, "hello");
}

TEST(PromptModeTest, ResolvePromptInputIgnoresWhitespaceOnlyContext) {
  std::istringstream stdin_stream("  \n\t");
  auto prompt = ResolvePromptInput({.prompt = "hello"}, &stdin_stream);
  ASSERT_TRUE(prompt.ok()) << prompt.status();
  EXPECT_EQ(*prompt, "hello");
}

TEST(PromptModeTest, ResolvePromptInputReadsFileInstructionAndPreservesContent) {
  const std::string path = ::testing::TempDir() + "/prompt_mode_file.md";
  {
    std::ofstream file(path);
    file << "line one\nline two\n";
  }

  std::istringstream stdin_stream;
  auto prompt = ResolvePromptInput({.prompt_file = path}, &stdin_stream);
  ASSERT_TRUE(prompt.ok()) << prompt.status();
  EXPECT_EQ(*prompt, "line one\nline two\n");
}

TEST(PromptModeTest, ResolvePromptInputCombinesStdinContextWithFileInstruction) {
  const std::string path = ::testing::TempDir() + "/prompt_mode_file.md";
  {
    std::ofstream file(path);
    file << "sort these files\n";
  }

  std::istringstream stdin_stream("b.cc\na.cc\n");
  auto prompt = ResolvePromptInput({.prompt_file = path}, &stdin_stream);
  ASSERT_TRUE(prompt.ok()) << prompt.status();
  EXPECT_EQ(*prompt, "Context:\nb.cc\na.cc\n\nInstruction:\nsort these files\n");
}

TEST(PromptModeTest, ResolvePromptInputRejectsDashPromptAlias) {
  std::istringstream stdin_stream("context\n");
  auto prompt = ResolvePromptInput({.prompt = "-"}, &stdin_stream);
  ASSERT_FALSE(prompt.ok());
  EXPECT_EQ(prompt.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(prompt.status().message(), "--prompt=- is not supported"));
}

TEST(PromptModeTest, ResolvePromptInputRejectsConflicts) {
  std::istringstream stdin_stream("stdin");
  auto prompt = ResolvePromptInput({.prompt = "hello", .prompt_file = "task.md"}, &stdin_stream);
  ASSERT_FALSE(prompt.ok());
  EXPECT_EQ(prompt.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(prompt.status().message(), "Specify exactly one instruction source"));
}

TEST(PromptModeTest, ResolvePromptInputRejectsMissingFile) {
  std::istringstream stdin_stream("ignored");
  auto prompt = ResolvePromptInput({.prompt_file = "/path/that/does/not/exist"}, &stdin_stream);
  ASSERT_FALSE(prompt.ok());
  EXPECT_EQ(prompt.status().code(), absl::StatusCode::kNotFound);
}

TEST(PromptModeTest, ResolvePromptInputRejectsEmptyPrompt) {
  std::istringstream stdin_stream("ignored");
  auto prompt = ResolvePromptInput({.prompt = "   \n\t"}, &stdin_stream);
  ASSERT_FALSE(prompt.ok());
  EXPECT_EQ(prompt.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(prompt.status().message(), "must not be empty"));
}

TEST(PromptModeTest, ResolvePromptInputRejectsEmptyFileInstruction) {
  const std::string path = ::testing::TempDir() + "/empty_prompt_mode_file.md";
  { std::ofstream file(path); }

  std::istringstream stdin_stream("context");
  auto prompt = ResolvePromptInput({.prompt_file = path}, &stdin_stream);
  ASSERT_FALSE(prompt.ok());
  EXPECT_EQ(prompt.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(PromptModeTest, HasStructuredFormatSourceDetectsAnySource) {
  EXPECT_FALSE(HasStructuredFormatSource({}));
  EXPECT_TRUE(HasStructuredFormatSource({.format = R"({"type":"object"})"}));
  EXPECT_TRUE(HasStructuredFormatSource({.format_file = "schema.json"}));
}

TEST(PromptModeTest, ResolveStructuredOutputSchemaAcceptsInlineSchema) {
  auto schema = ResolveStructuredOutputSchema(
      {.format = R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})"});
  ASSERT_TRUE(schema.ok()) << schema.status();
  EXPECT_EQ(json_get_or<std::string>(*schema, "type", ""), "object");
}

TEST(PromptModeTest, ResolveStructuredOutputSchemaAcceptsFileSchema) {
  const std::string path = ::testing::TempDir() + "/structured_schema.json";
  {
    std::ofstream file(path);
    file << R"({"type":"object","properties":{"ok":{"type":"boolean"}}})";
  }

  auto schema = ResolveStructuredOutputSchema({.format_file = path});
  ASSERT_TRUE(schema.ok()) << schema.status();
  EXPECT_EQ(json_get_or<std::string>(*schema, "type", ""), "object");
}

TEST(PromptModeTest, ResolveStructuredOutputSchemaRejectsConflicts) {
  auto schema = ResolveStructuredOutputSchema({.format = R"({"type":"object"})", .format_file = "schema.json"});
  ASSERT_FALSE(schema.ok());
  EXPECT_EQ(schema.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(PromptModeTest, ResolveStructuredOutputSchemaRejectsMalformedJson) {
  auto schema = ResolveStructuredOutputSchema({.format = R"({"type":"object")"});
  ASSERT_FALSE(schema.ok());
  EXPECT_EQ(schema.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(PromptModeTest, ResolveStructuredOutputSchemaRejectsUnsupportedSchema) {
  auto schema = ResolveStructuredOutputSchema({.format = R"({"type":"object","oneOf":[]})"});
  ASSERT_FALSE(schema.ok());
  EXPECT_EQ(schema.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(PromptModeTest, ValidatePromptModePreflightRejectsFormatWithoutPrompt) {
  absl::Status status = ValidatePromptModePreflight({}, {.format = R"({"type":"object"})"}, "text");
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(status.message(), "require prompt mode"));
}

TEST(PromptModeTest, ValidatePromptModePreflightRejectsFormatWithJsonOutput) {
  absl::Status status = ValidatePromptModePreflight({.prompt = "hello"}, {.format = R"({"type":"object"})"}, "json");
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(status.message(), "--output=json"));
}

TEST(PromptModeTest, ValidatePromptModePreflightAllowsFormatWithTextPromptOutput) {
  EXPECT_TRUE(ValidatePromptModePreflight({.prompt = "hello"}, {.format = R"({"type":"object"})"}, "text").ok());
}

TEST(PromptModeTest, ValidatePromptOutputModeAcceptsTextAndJson) {
  EXPECT_TRUE(ValidatePromptOutputMode("text").ok());
  EXPECT_TRUE(ValidatePromptOutputMode("json").ok());
}

TEST(PromptModeTest, ValidatePromptOutputModeRejectsUnknownMode) {
  absl::Status status = ValidatePromptOutputMode("xml");
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(status.message(), "Invalid --output value"));
}

TEST(PromptModeTest, PromptRunResultToJsonSerializesSuccessShape) {
  InteractionEngine::PromptRunResult result;
  result.ok = true;
  result.session_id = "session";
  result.model = "model";
  result.active_skills = {"c++_expert"};
  result.assistant_message = "answer";
  result.duration_ms = 123;

  auto parsed = json_parse(PromptRunResultToJson(result));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(json_get_or<bool>(*parsed, "ok", false), true);
  EXPECT_EQ(json_get_or<std::string>(*parsed, "session", ""), "session");
  EXPECT_EQ(json_get_or<std::string>(*parsed, "model", ""), "model");
  EXPECT_EQ(json_get_or<std::string>(*parsed, "assistant_message", ""), "answer");
  const nlohmann::json* structured_output = json_at(*parsed, "structured_output");
  ASSERT_NE(structured_output, nullptr);
  EXPECT_TRUE(structured_output->is_null());
  EXPECT_EQ(json_get_or<int>(*parsed, "duration_ms", 0), 123);
  const nlohmann::json* error = json_at(*parsed, "error");
  ASSERT_NE(error, nullptr);
  EXPECT_TRUE(error->is_null());
}

TEST(PromptModeTest, PromptRunResultToJsonSerializesStructuredOutput) {
  InteractionEngine::PromptRunResult result;
  result.ok = true;
  result.structured_output = nlohmann::json{{"answer", "42"}};

  auto parsed = json_parse(PromptRunResultToJson(result));
  ASSERT_TRUE(parsed.has_value());
  const nlohmann::json* structured_output = json_at(*parsed, "structured_output");
  ASSERT_NE(structured_output, nullptr);
  ASSERT_TRUE(structured_output->is_object());
  EXPECT_EQ(json_get_or<std::string>(*structured_output, "answer", ""), "42");
}

TEST(PromptModeTest, PromptRunResultToJsonSerializesFailureShape) {
  InteractionEngine::PromptRunResult result;
  result.ok = false;
  result.session_id = "session";
  result.model = "model";
  result.error_code = "invalid_argument";
  result.error_message = "bad input";

  auto parsed = json_parse(PromptRunResultToJson(result));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(json_get_or<bool>(*parsed, "ok", true), false);
  const nlohmann::json* error = json_at(*parsed, "error");
  ASSERT_NE(error, nullptr);
  ASSERT_TRUE(error->is_object());
  EXPECT_EQ(json_get_or<std::string>(*error, "code", ""), "invalid_argument");
  EXPECT_EQ(json_get_or<std::string>(*error, "message", ""), "bad input");
}

}  // namespace
}  // namespace slop
