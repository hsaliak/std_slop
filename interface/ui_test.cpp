#include "interface/ui.h"

#include <iostream>
#include <sstream>
#include <string>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "gtest/gtest.h"

#include "interface/color.h"
#include "interface/terminal.h"
namespace slop {
TEST(UiTest, GetTerminalWidth) {
  size_t width = slop::GetTerminalWidth();
  EXPECT_GT(width, 0);
}
TEST(UiTest, WrapTextBasic) {
  std::string text = "Hello world";
  std::string wrapped = slop::WrapText(text, 20);
  EXPECT_EQ(wrapped, "Hello world");
}
TEST(UiTest, WrapTextLong) {
  std::string text = "This is a longer string that should be wrapped.";
  std::string wrapped = slop::WrapText(text, 10);
  // Expect it to be wrapped into multiple lines
  EXPECT_TRUE(absl::StrContains(wrapped, "\n"));
}
TEST(UiTest, WrapTextWithPrefix) {
  std::string text = "Line one\nLine two";
  std::string prefix = "> ";
  std::string wrapped = slop::WrapText(text, 80, prefix);
  EXPECT_TRUE(absl::StrContains(wrapped, "> Line one"));
  EXPECT_TRUE(absl::StrContains(wrapped, "> Line two"));
}

TEST(UiTest, GetHelpTextOmitsCoreOperationsWhenRequested) {
  const std::string help_text = GetHelpText(false);
  EXPECT_FALSE(absl::StrContains(help_text, "### Core Operations"));
}

TEST(UiTest, GetHelpTextIncludesCoreOperationsByDefault) {
  const std::string help_text = GetHelpText();
  EXPECT_TRUE(absl::StrContains(help_text, "### Core Operations"));
}

TEST(UiTest, GetCliHelpTextOmitsSlashCommandsSection) {
  const std::string help_text = GetCliHelpText();
  EXPECT_FALSE(absl::StrContains(help_text, "## Slash Commands"));
  EXPECT_FALSE(absl::StrContains(help_text, "/help"));
}
TEST(UiTest, PrintAssistantMessageBasic) {
  std::string content = "Hello, user!";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintAssistantMessage(content);
  std::cout.rdbuf(old);
  std::string output = buffer.str();
  EXPECT_TRUE(absl::StrContains(output, "Hello, user!"));
}
TEST(UiTest, PrintAssistantMessageWithSpecialHeaders) {
  // Special headers should be rendered with semantic colors.
  std::string content = "### THOUGHT\nI am thinking.\n\n### STATE\nGoal: test\n\nHello, user!";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintAssistantMessage(content);
  std::cout.rdbuf(old);
  std::string output = buffer.str();
  // Verify it contains both headers and the content
  // Since ANSI codes might split the string (e.g., ### [ANSI] THOUGHT),
  // we check for the components.
  EXPECT_TRUE(absl::StrContains(output, "###"));
  EXPECT_TRUE(absl::StrContains(output, "THOUGHT"));
  EXPECT_TRUE(absl::StrContains(output, "I am thinking."));
  EXPECT_TRUE(absl::StrContains(output, "STATE"));
  EXPECT_TRUE(absl::StrContains(output, "Goal: test"));
  EXPECT_TRUE(absl::StrContains(output, "Hello, user!"));
  // Verify color codes
  // Grey/Thought
  EXPECT_TRUE(absl::StrContains(output, ansi::Grey));
  // Yellow/State
  EXPECT_TRUE(absl::StrContains(output, ansi::Yellow));
  // White/Assistant
  EXPECT_TRUE(absl::StrContains(output, ansi::White));
}
TEST(UiTest, PrintAssistantMessageWithPrefix) {
  std::string content = "Hello world";
  std::string prefix = "  ";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintAssistantMessage(content, prefix);
  std::cout.rdbuf(old);
  std::string output = buffer.str();
  EXPECT_TRUE(absl::StrContains(output, "Hello world"));
}
TEST(UiTest, PrintAssistantMessageWithTokens) {
  std::string content = "Hello world";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintAssistantMessage(content, "", 123);
  std::cout.rdbuf(old);
  std::string output = buffer.str();
  EXPECT_TRUE(absl::StrContains(output, "123 tokens"));
}
TEST(UiTest, PrintAssistantMessageWithTokensAndPrefix) {
  std::string content = "Hello world";
  std::string prefix = "  ";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintAssistantMessage(content, prefix, 123);
  std::cout.rdbuf(old);
  std::string output = buffer.str();
  EXPECT_TRUE(absl::StrContains(output, "123 tokens"));
  // Check for the prefix and bullet, allowing for ANSI codes
  EXPECT_TRUE(absl::StrContains(output, "      "));
  EXPECT_TRUE(absl::StrContains(output, "· 123 tokens"));
}
TEST(UiTest, FlattenJsonArgs) {
  EXPECT_EQ(FlattenJsonArgs("{}"), "");
  EXPECT_EQ(FlattenJsonArgs("{\"path\": \"foo.txt\"}"), "path: \"foo.txt\"");
  EXPECT_EQ(FlattenJsonArgs("{\"a\": 1, \"b\": \"c\"}"), "a: 1 | b: \"c\"");
  EXPECT_EQ(FlattenJsonArgs("invalid"), "invalid");
}
TEST(UiTest, PrintToolCallMessage) {
  std::string name = "test_tool";
  std::string args = R"({"query": "test"})";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolCallMessage(name, args);
  std::cout.rdbuf(old);
  std::string output = buffer.str();
  EXPECT_TRUE(absl::StrContains(output, "test_tool"));
  EXPECT_TRUE(absl::StrContains(output, "❯"));
  EXPECT_TRUE(absl::StrContains(output, "query: \"test\""));
}
TEST(UiTest, PrintToolCallMessageWithTokens) {
  std::string name = "test_tool";
  std::string args = R"({"query": "test"})";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolCallMessage(name, args, "", 123);
  std::cout.rdbuf(old);
  std::string output = buffer.str();
  EXPECT_TRUE(absl::StrContains(output, "test_tool"));
  EXPECT_TRUE(absl::StrContains(output, "· 123 tokens"));
}
TEST(UiTest, PrintToolResultMessage) {
  std::string name = "test_tool";
  std::string result = "Success!";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolResultMessage(name, result);
  std::cout.rdbuf(old);
  std::string output = buffer.str();
  EXPECT_TRUE(absl::StrContains(output, "│"));
  EXPECT_TRUE(absl::StrContains(output, "completed"));
}
TEST(UiTest, PrintToolResultMessageNoPreview) {
  std::string name = "test_tool";
  std::string result = "line 1\nline 2\nline 3\nline 4";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolResultMessage(name, result, "completed");
  std::cout.rdbuf(old);
  std::string output = buffer.str();
  EXPECT_TRUE(absl::StrContains(output, "│"));
  EXPECT_TRUE(absl::StrContains(output, "completed (4 lines)"));
  EXPECT_TRUE(absl::StrContains(output, "line 1"));
  EXPECT_TRUE(!absl::StrContains(output, "..."));
}
TEST(UiTest, PrintToolResultMessageStderr) {
  std::string name = "test_tool";
  std::string result = "stdout line 1\n### STDERR\nstderr line 1\nstderr line 2";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolResultMessage(name, result, "completed");
  std::cout.rdbuf(old);
  std::string output = buffer.str();
  EXPECT_TRUE(absl::StrContains(output, "stdout line 1"));
  EXPECT_TRUE(absl::StrContains(output, "stderr line 1"));
  EXPECT_TRUE(absl::StrContains(output, "stderr line 2"));
}

TEST(UiTest, PrintToolResultMessageUnknownToolJsonDefaultFormatting) {
  std::string name = "custom_persisted_tool";
  std::string result = R"({"alpha":1,"beta":"two"})";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolResultMessage(name, result, "completed");
  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(absl::StrContains(output, "```json"));
  EXPECT_TRUE(absl::StrContains(output, "\"alpha\": 1"));
  EXPECT_TRUE(absl::StrContains(output, "\"beta\": \"two\""));
}

TEST(UiTest, PrintToolResultMessageEnvelopeJsonFormatting) {
  std::string name = "dynamic_tool";
  std::string result =
      R"({"ok":true,"tool":"list_directory","requested_tool":"list_directory","alias_used":false,"result":"File: a.txt"})";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolResultMessage(name, result, "completed");
  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(absl::StrContains(output, "ok"));
  EXPECT_TRUE(absl::StrContains(output, "tool"));
  EXPECT_TRUE(absl::StrContains(output, "list_directory"));
  EXPECT_TRUE(absl::StrContains(output, "Result"));
  EXPECT_TRUE(absl::StrContains(output, "File: a.txt"));
}

TEST(UiTest, PrintToolResultMessageEnvelopeJsonErrorFormatting) {
  std::string name = "dynamic_tool";
  std::string result = R"({"ok":false,"error":{"type":"TOOL_ERROR","message":"boom"}})";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolResultMessage(name, result, "error");
  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(absl::StrContains(output, "error"));
  EXPECT_TRUE(absl::StrContains(output, "Error"));
  EXPECT_TRUE(absl::StrContains(output, "TOOL_ERROR"));
  EXPECT_TRUE(absl::StrContains(output, "boom"));
}

TEST(UiTest, PrintToolResultMessageExecuteBashEnvelopeFormatsOutputField) {
  std::string name = "execute_bash";
  std::string result =
      R"({"stdout":"line1\nline2","stderr":"","exit_code":0,"output":"line1\nline2","command":"echo hi","executed_command":"echo hi"})";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolResultMessage(name, result, "completed");
  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(absl::StrContains(output, "exit_code"));
  EXPECT_TRUE(absl::StrContains(output, "Output"));
  EXPECT_TRUE(absl::StrContains(output, "line1"));
  EXPECT_TRUE(absl::StrContains(output, "line2"));
}

TEST(UiTest, PrintToolResultMessageNestedJsonStringFormatting) {
  std::string name = "dynamic_tool";
  std::string result = R"("{\"alpha\":1,\"beta\":\"two\"}")";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolResultMessage(name, result, "completed");
  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(absl::StrContains(output, "alpha"));
  EXPECT_TRUE(absl::StrContains(output, "beta"));
  EXPECT_TRUE(absl::StrContains(output, "two"));
}

TEST(UiTest, PrintToolResultMessageNestedJsonStringTruncationMarker) {
  std::string name = "dynamic_tool";
  std::string result =
      "\"{\\\"k01\\\":1,\\\"k02\\\":2,\\\"k03\\\":3,\\\"k04\\\":4,\\\"k05\\\":5,\\\"k06\\\":6,\\\"k07\\\":7,"
      "\\\"k08\\\":8,\\\"k09\\\":9,\\\"k10\\\":10,\\\"k11\\\":11,\\\"k12\\\":12,\\\"k13\\\":13,\\\"k14\\\":14,"
      "\\\"k15\\\":15,\\\"k16\\\":16,\\\"k17\\\":17,\\\"k18\\\":18,\\\"k19\\\":19,\\\"k20\\\":20}\"";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolResultMessage(name, result, "completed");
  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(absl::StrContains(output, "... (truncated)"));
}

TEST(UiTest, PrintToolResultMessagePatchToolRendersDiffBlock) {
  std::string name = "patch_tool";
  std::string result = R"({"ok":true,"mode":"apply","path":"foo.txt","applied":1,"unified_diff":"--- foo.txt\n+++ foo.txt\n@@ -1 +1 @@\n-old\n+new\n"})";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolResultMessage(name, result, "completed");
  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(absl::StrContains(output, "mode"));
  EXPECT_TRUE(absl::StrContains(output, "apply"));
  EXPECT_TRUE(absl::StrContains(output, "Diff"));
  EXPECT_TRUE(absl::StrContains(output, "--- foo.txt"));
  EXPECT_TRUE(absl::StrContains(output, "+++ foo.txt"));
  EXPECT_TRUE(absl::StrContains(output, "@@ -1 +1 @@"));
  EXPECT_TRUE(absl::StrContains(output, "+new"));
}

TEST(UiTest, PrintToolResultMessagePatchToolLongDiffTruncatesWithExistingRules) {
  std::string name = "patch_tool";
  std::string long_diff;
  for (int i = 0; i < 30; ++i) {
    absl::StrAppend(&long_diff, "@@ -", i + 1, " +", i + 1, " @@\n-", i, "\n+", i + 1, "\n");
  }
  std::string escaped_diff = long_diff;
  absl::StrReplaceAll({{"\\", "\\\\"}, {"\"", "\\\""}, {"\n", "\\n"}}, &escaped_diff);
  std::string payload =
      absl::StrCat("{\"ok\":true,\"mode\":\"apply\",\"path\":\"foo.txt\",\"applied\":30,\"unified_diff\":\"",
                   escaped_diff, "\"}");
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolResultMessage(name, payload, "completed");
  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(absl::StrContains(output, "... (truncated)"));
}

TEST(UiTest, PrintToolResultMessagePatchToolWithoutDiffFallsBack) {
  std::string name = "patch_tool";
  std::string result = R"({"ok":true,"mode":"apply","path":"foo.txt","applied":1})";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolResultMessage(name, result, "completed");
  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(absl::StrContains(output, "ok"));
  EXPECT_TRUE(absl::StrContains(output, "completed"));
  EXPECT_TRUE(!absl::StrContains(output, "### Diff"));
}

TEST(UiTest, PrintToolResultMessageHTTPError) {
  std::string name = "test_tool";
  std::string result = "Error: HTTP 429 Too Many Requests\nRate limit exceeded";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolResultMessage(name, result, "error");
  std::cout.rdbuf(old);
  std::string output = buffer.str();
  EXPECT_TRUE(absl::StrContains(output, "HTTP 429 Too Many Requests"));
  EXPECT_TRUE(absl::StrContains(output, "Rate limit exceeded"));
}
TEST(UiTest, PrintToolResultMessageResourceExhausted) {
  std::string name = "test_tool";
  std::string result = "Error: RESOURCE_EXHAUSTED: Quota exceeded";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolResultMessage(name, result, "error");
  std::cout.rdbuf(old);
  std::string output = buffer.str();
  EXPECT_TRUE(absl::StrContains(output, "RESOURCE_EXHAUSTED"));
}
TEST(UiTest, PrintToolResultMessage503Error) {
  std::string name = "test_tool";
  std::string result = "Error: 503 Service Unavailable";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolResultMessage(name, result, "error");
  std::cout.rdbuf(old);
  std::string output = buffer.str();
  EXPECT_TRUE(absl::StrContains(output, "503 Service Unavailable"));
}
TEST(UiTest, PrintToolResultMessageQuotaError) {
  std::string name = "test_tool";
  std::string result = R"(Error: {
  "error": {
    "code": 429,
    "message": "You have exhausted your capacity on this model. Your quota will reset after 0s.",
    "status": "RESOURCE_EXHAUSTED"
  }
})";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolResultMessage(name, result, "error");
  std::cout.rdbuf(old);
  std::string output = buffer.str();
  EXPECT_TRUE(absl::StrContains(output, "exhausted your capacity"));
  EXPECT_TRUE(absl::StrContains(output, "RESOURCE_EXHAUSTED"));
}
TEST(UiTest, RenderMarkdownWithJsCodeBlock) {
  // Test the markdown rendering directly
  std::string markdown = "```javascript\nlocal x = 1\nprint(x)\n```";
  std::string rendered;
  RenderMarkdown(markdown, "", &rendered);
  // The rendered output should contain the lua code
  EXPECT_TRUE(absl::StrContains(rendered, "local"));
  EXPECT_TRUE(absl::StrContains(rendered, "x"));
}

TEST(UiTest, RenderMarkdownPlainTargetHasNoAnsiEscapes) {
  std::string markdown = "### Title\n\n**bold** text";
  std::string rendered;
  RenderMarkdown(markdown, "", &rendered, RenderTarget::kPlainText);
  EXPECT_TRUE(absl::StrContains(rendered, "Title"));
  EXPECT_TRUE(absl::StrContains(rendered, "bold"));
  EXPECT_FALSE(absl::StrContains(rendered, "\033["));
}

TEST(UiTest, RenderMarkdownPlainTargetTableHasNoAnsiEscapes) {
  std::string markdown = "| Name | Description |\n|---|---|\n| tool | **bold** cell |\n";
  std::string rendered;
  RenderMarkdown(markdown, "", &rendered, RenderTarget::kPlainText);
  EXPECT_TRUE(absl::StrContains(rendered, "Name"));
  EXPECT_TRUE(absl::StrContains(rendered, "bold"));
  EXPECT_FALSE(absl::StrContains(rendered, "\033["));
}
}  // namespace slop
