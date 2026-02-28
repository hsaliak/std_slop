#include "interface/ui.h"
#include "interface/terminal.h"

#include <iostream>
#include <sstream>
#include <string>

#include "absl/strings/match.h"
#include "gtest/gtest.h"

#include "interface/color.h"
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
TEST(UiTest, PrintToolCallMessageRunJs) {
  std::string name = "run_js";
  std::string args = R"raw({"script": "print('hello')\nprint('world')" })raw";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolCallMessage(name, args);
  std::cout.rdbuf(old);
  std::string output = buffer.str();
  EXPECT_TRUE(absl::StrContains(output, "run_js (control plane)"));
  EXPECT_TRUE(absl::StrContains(output, "```javascript"));
  EXPECT_TRUE(absl::StrContains(output, "print"));
}
TEST(UiTest, PrintToolCallMessageRunJsLong) {
  std::string name = "run_js";
  std::string args = R"raw({"script": "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11" })raw";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolCallMessage(name, args);
  std::cout.rdbuf(old);
  std::string output = buffer.str();
  EXPECT_TRUE(absl::StrContains(output, "```javascript"));
  EXPECT_TRUE(absl::StrContains(output, "10"));
  EXPECT_TRUE(absl::StrContains(output, "11"));
}
TEST(UiTest, PrintToolCallMessageRunJsRendersMarkdown) {
  std::string name = "run_js";
  std::string args = R"raw({"script": "local x = 1\nprint(x)"})raw";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolCallMessage(name, args);
  std::cout.rdbuf(old);
  std::string output = buffer.str();
  // Should contain the markdown code fence with lua
  EXPECT_TRUE(absl::StrContains(output, "```javascript"));
  EXPECT_TRUE(absl::StrContains(output, "run_js"));
  EXPECT_TRUE(absl::StrContains(output, "control plane"));
}
TEST(UiTest, PrintToolCallMessageRunJsWithFunction) {
  std::string name = "run_js";
  std::string args = R"raw({"script": "local function hello()\n  print('world')\nend\nhello()"})raw";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
  PrintToolCallMessage(name, args);
  std::cout.rdbuf(old);
  std::string output = buffer.str();
  // New behavior: renders as markdown with code fence
  EXPECT_TRUE(absl::StrContains(output, "```javascript"));
  EXPECT_TRUE(absl::StrContains(output, "local"));
  EXPECT_TRUE(absl::StrContains(output, "function"));
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
}  // namespace slop
