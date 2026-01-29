#include "interface/ui.h"

#include <iostream>
#include <sstream>
#include <string>

#include "gtest/gtest.h"

#include "interface/color.h"

namespace slop {

TEST(UiTest, GetTerminalWidth) {
  size_t width = GetTerminalWidth();
  EXPECT_GT(width, 0);
}

TEST(UiTest, WrapTextBasic) {
  std::string text = "Hello world";
  std::string wrapped = WrapText(text, 20);
  EXPECT_EQ(wrapped, "Hello world");
}

TEST(UiTest, WrapTextLong) {
  std::string text = "This is a longer string that should be wrapped.";
  std::string wrapped = WrapText(text, 10);
  // Expect it to be wrapped into multiple lines
  EXPECT_TRUE(wrapped.find("\n") != std::string::npos);
}

TEST(UiTest, WrapTextWithPrefix) {
  std::string text = "Line one\nLine two";
  std::string prefix = "> ";
  std::string wrapped = WrapText(text, 80, prefix);
  EXPECT_TRUE(wrapped.find("> Line one") != std::string::npos);
  EXPECT_TRUE(wrapped.find("> Line two") != std::string::npos);
}

TEST(UiTest, PrintAssistantMessageBasic) {
  std::string content = "Hello, user!";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

  PrintAssistantMessage(content);

  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(output.find("Hello, user!") != std::string::npos);
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
  EXPECT_TRUE(output.find("###") != std::string::npos);
  EXPECT_TRUE(output.find("THOUGHT") != std::string::npos);
  EXPECT_TRUE(output.find("I am thinking.") != std::string::npos);
  EXPECT_TRUE(output.find("STATE") != std::string::npos);
  EXPECT_TRUE(output.find("Goal: test") != std::string::npos);
  EXPECT_TRUE(output.find("Hello, user!") != std::string::npos);

  // Verify color codes
  // Grey/Thought: \033[90m
  EXPECT_TRUE(output.find("\033[90m") != std::string::npos);
  // Yellow/State: \033[33m
  EXPECT_TRUE(output.find("\033[33m") != std::string::npos);
  // White/Assistant: \033[37m
  EXPECT_TRUE(output.find("\033[37m") != std::string::npos);
}

TEST(UiTest, PrintAssistantMessageWithPrefix) {
  std::string content = "Hello world";
  std::string prefix = "  ";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

  PrintAssistantMessage(content, prefix);

  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(output.find("Hello world") != std::string::npos);
}

TEST(UiTest, PrintAssistantMessageWithTokens) {
  std::string content = "Hello world";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

  PrintAssistantMessage(content, "", 123);

  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(output.find("123 tokens") != std::string::npos);
}

TEST(UiTest, PrintAssistantMessageWithTokensAndPrefix) {
  std::string content = "Hello world";
  std::string prefix = "  ";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

  PrintAssistantMessage(content, prefix, 123);

  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(output.find("123 tokens") != std::string::npos);
  // Check for the prefix and bullet, allowing for ANSI codes
  EXPECT_TRUE(output.find("      ") != std::string::npos);
  EXPECT_TRUE(output.find("· 123 tokens") != std::string::npos);
}

TEST(UiTest, FlattenJsonArgs) {
  EXPECT_EQ(FlattenJsonArgs("{}"), "");
  EXPECT_EQ(FlattenJsonArgs("{\"path\": \"foo.txt\"}"), "path: \"foo.txt\"");
  EXPECT_EQ(FlattenJsonArgs("{\"a\": 1, \"b\": \"c\"}"), "a: 1 | b: \"c\"");
  EXPECT_EQ(FlattenJsonArgs("invalid"), "invalid");
}

TEST(UiTest, PrintToolCallMessage) {
  std::string name = "test_tool";
  std::string args = "{\"query\": \"test\"}";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

  PrintToolCallMessage(name, args);

  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(output.find("test_tool") != std::string::npos);
  EXPECT_TRUE(output.find("❯") != std::string::npos);
  EXPECT_TRUE(output.find("query: \"test\"") != std::string::npos);
}

TEST(UiTest, PrintToolResultMessage) {
  std::string name = "test_tool";
  std::string result = "Success!";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

  PrintToolResultMessage(name, result);

  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(output.find("┗━") != std::string::npos);
  EXPECT_TRUE(output.find("completed") != std::string::npos);
}

TEST(UiTest, PrintToolResultMessageTruncated) {
  std::string name = "test_tool";
  std::string result = "line 1\nline 2\nline 3\nline 4";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

  PrintToolResultMessage(name, result, "completed");

  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(output.find("┗━") != std::string::npos);
  EXPECT_TRUE(output.find("completed (4 lines)") != std::string::npos);
  EXPECT_TRUE(output.find("line 1") == std::string::npos);
}

TEST(UiTest, PrintToolResultMessageExactLines) {
  std::string name = "test_tool";
  std::string exact_result = "line 1\nline 2\nline 3";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

  PrintToolResultMessage("test_tool", exact_result, "completed");

  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(output.find("┗━") != std::string::npos);
  EXPECT_TRUE(output.find("completed (3 lines)") != std::string::npos);
  EXPECT_TRUE(output.find("line 1") == std::string::npos);
}

}  // namespace slop
