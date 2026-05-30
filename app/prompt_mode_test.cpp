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
  EXPECT_TRUE(HasPromptInputSource({.prompt_stdin = true}));
}

TEST(PromptModeTest, ResolvePromptInputUsesLiteralPrompt) {
  std::istringstream stdin_stream("ignored");
  auto prompt = ResolvePromptInput({.prompt = "hello"}, &stdin_stream);
  ASSERT_TRUE(prompt.ok()) << prompt.status();
  EXPECT_EQ(*prompt, "hello");
}

TEST(PromptModeTest, ResolvePromptInputReadsFileAndPreservesContent) {
  const std::string path = ::testing::TempDir() + "/prompt_mode_file.md";
  {
    std::ofstream file(path);
    file << "line one\nline two\n";
  }

  std::istringstream stdin_stream("ignored");
  auto prompt = ResolvePromptInput({.prompt_file = path}, &stdin_stream);
  ASSERT_TRUE(prompt.ok()) << prompt.status();
  EXPECT_EQ(*prompt, "line one\nline two\n");
}

TEST(PromptModeTest, ResolvePromptInputReadsPromptStdin) {
  std::istringstream stdin_stream("from stdin\n");
  auto prompt = ResolvePromptInput({.prompt_stdin = true}, &stdin_stream);
  ASSERT_TRUE(prompt.ok()) << prompt.status();
  EXPECT_EQ(*prompt, "from stdin\n");
}

TEST(PromptModeTest, ResolvePromptInputReadsPromptDashFromStdin) {
  std::istringstream stdin_stream("from dash\n");
  auto prompt = ResolvePromptInput({.prompt = "-"}, &stdin_stream);
  ASSERT_TRUE(prompt.ok()) << prompt.status();
  EXPECT_EQ(*prompt, "from dash\n");
}

TEST(PromptModeTest, ResolvePromptInputRejectsConflicts) {
  std::istringstream stdin_stream("stdin");
  auto prompt = ResolvePromptInput({.prompt = "hello", .prompt_file = "task.md"}, &stdin_stream);
  ASSERT_FALSE(prompt.ok());
  EXPECT_EQ(prompt.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(prompt.status().message(), "Specify exactly one prompt source"));
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
  EXPECT_EQ(json_get_or<int>(*parsed, "duration_ms", 0), 123);
  const nlohmann::json* error = json_at(*parsed, "error");
  ASSERT_NE(error, nullptr);
  EXPECT_TRUE(error->is_null());
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
