#include "interface/ui.h"

#include <cstdlib>
#include <iostream>
#include <sstream>

#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include <gtest/gtest.h>

namespace slop {

TEST(UiTest, WrapTextSimple) {
  std::string text = "This is a long sentence that should be wrapped into multiple lines because it exceeds the width.";
  std::string wrapped = WrapText(text, 20);
  std::stringstream ss(wrapped);
  std::string line;
  while (std::getline(ss, line)) {
    EXPECT_LE(line.length(), 20);
  }
}

TEST(UiTest, WrapTextAnsi) {
  std::string red_hello = "\033[31mHello\033[0m";
  std::string text = red_hello + " world " + red_hello + " again";
  std::string wrapped = WrapText(text, 12);
  std::stringstream ss(wrapped);
  std::string line;
  std::getline(ss, line);
  EXPECT_EQ(line, red_hello + " world");
  std::getline(ss, line);
  EXPECT_EQ(line, red_hello + " again");
}

TEST(UiTest, PrintJsonAsTableEmpty) {
  std::string empty_json = "[]";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

  auto status = PrintJsonAsTable(empty_json);

  std::cout.rdbuf(old);
  EXPECT_TRUE(status.ok());
  EXPECT_TRUE(buffer.str().find("No results found.") != std::string::npos);
}

TEST(UiTest, PrintJsonAsTableValid) {
  std::string json = R"([{"id": 1, "name": "test"}, {"id": 2, "name": "example"}])";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

  auto status = PrintJsonAsTable(json);

  std::cout.rdbuf(old);
  EXPECT_TRUE(status.ok());
  EXPECT_TRUE(buffer.str().find("id") != std::string::npos);
  EXPECT_TRUE(buffer.str().find("name") != std::string::npos);
  EXPECT_TRUE(buffer.str().find("test") != std::string::npos);
  EXPECT_TRUE(buffer.str().find("example") != std::string::npos);
}

TEST(UiTest, PrintJsonAsTableInvalid) {
  std::string invalid_json = "{not json}";
  auto status = PrintJsonAsTable(invalid_json);
  EXPECT_FALSE(status.ok());
}

TEST(UiTest, FormatAssembledContextGemini) {
  std::string json = R"({
        "contents": [
            {"role": "user", "parts": [{"text": "Hello!"}]}
        ],
        "system_instruction": {"parts": [{"text": "You are a helper."}]}
    })";
  std::string formatted = FormatAssembledContext(json);
  EXPECT_TRUE(formatted.find("SYSTEM INSTRUCTION:") != std::string::npos);
  EXPECT_TRUE(formatted.find("You are a helper.") != std::string::npos);
  EXPECT_TRUE(formatted.find("Role: user") != std::string::npos);
  EXPECT_TRUE(formatted.find("Hello!") != std::string::npos);
}

TEST(UiTest, FormatAssembledContextGCA) {
  std::string json = R"({
        "request": {
            "contents": [
                {"role": "user", "parts": [{"text": "GCA hello!"}]}
            ],
            "system_instruction": {"parts": [{"text": "GCA system prompt."}]}
        },
        "model": "model-id"
    })";
  std::string formatted = FormatAssembledContext(json);
  EXPECT_TRUE(formatted.find("SYSTEM INSTRUCTION:") != std::string::npos);
  EXPECT_TRUE(formatted.find("GCA system prompt.") != std::string::npos);
  EXPECT_TRUE(formatted.find("Role: user") != std::string::npos);
  EXPECT_TRUE(formatted.find("GCA hello!") != std::string::npos);
}

TEST(UiTest, FormatAssembledContextOpenAI) {
  std::string json = R"({
        "messages": [
            {"role": "system", "content": "OpenAI System"},
            {"role": "user", "content": "OpenAI User"},
            {"role": "assistant", "content": null, "tool_calls": [{"id": "call_1", "function": {"name": "test_tool"}}]}
        ]
    })";
  std::string formatted = FormatAssembledContext(json);
  EXPECT_TRUE(formatted.find("OpenAI System") != std::string::npos);
  EXPECT_TRUE(formatted.find("Role: user") != std::string::npos);
  EXPECT_TRUE(formatted.find("OpenAI User") != std::string::npos);
  EXPECT_TRUE(formatted.find("Role: assistant") != std::string::npos);
  EXPECT_TRUE(formatted.find("Tool Calls:") != std::string::npos);
  EXPECT_TRUE(formatted.find("test_tool") != std::string::npos);
}

TEST(UiTest, SmartDisplayFallback) {
  const char* old_editor = std::getenv("EDITOR");
  unsetenv("EDITOR");

  std::string test_content = "fallback content test";
  std::stringstream buffer;
  std::streambuf* old_cout = std::cout.rdbuf(buffer.rdbuf());

  SmartDisplay(test_content);

  std::cout.rdbuf(old_cout);
  if (old_editor) setenv("EDITOR", old_editor, 1);

  EXPECT_TRUE(buffer.str().find(test_content) != std::string::npos);
}

TEST(UiTest, GetTerminalWidth) {
  size_t width = GetTerminalWidth();
  EXPECT_GT(width, 0);
}

TEST(UiTest, PrintToolCallMessageShowsSummary) {
  std::string args = "{\"query\": \"foo\", \"path\": \"bar\"}";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

  PrintToolCallMessage("test_tool", args);

  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(output.find("test_tool") != std::string::npos);
  EXPECT_TRUE(output.find("\"query\":\"foo\"") != std::string::npos || 
              output.find("\"query\": \"foo\"") != std::string::npos);
}

TEST(UiTest, PrintToolResultMessageShowsOnlySummary) {
  std::string long_result = "line 1\nline 2\nline 3\nline 4\nline 5";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

  PrintToolResultMessage("test_tool", long_result, "completed", "  ");

  std::cout.rdbuf(old);
  std::string output = buffer.str();

  // Verify header summary is present and indented (prefix "  " + 8 spaces)
  EXPECT_TRUE(absl::StartsWith(output, "          "));
  EXPECT_TRUE(output.find("test_tool") != std::string::npos);
  EXPECT_TRUE(output.find("5 lines") != std::string::npos);

  // Verify that it DOES NOT contain any of the result lines
  EXPECT_TRUE(output.find("line 1") == std::string::npos);
  EXPECT_TRUE(output.find("line 2") == std::string::npos);
  EXPECT_TRUE(output.find("line 3") == std::string::npos);
  EXPECT_TRUE(output.find("line 4") == std::string::npos);
  EXPECT_TRUE(output.find("line 5") == std::string::npos);
}

TEST(UiTest, PrintToolResultMessageShowsOnlySummaryExact) {
  std::string exact_result = "line 1\nline 2\nline 3";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

  PrintToolResultMessage("test_tool", exact_result, "completed");

  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(output.find("test_tool (completed) - 3 lines") != std::string::npos);
  EXPECT_TRUE(output.find("line 1") == std::string::npos);
}

TEST(UiTest, PrintAssistantMessageWithThoughts) {
  std::string content = "---THOUGHT---\nI am thinking.\n---\nHello, user!";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

  PrintAssistantMessage(content);

  std::cout.rdbuf(old);
  std::string output = buffer.str();

  // Verify it contains both the thought and the message
  EXPECT_TRUE(output.find("I am thinking.") != std::string::npos);
  EXPECT_TRUE(output.find("Hello, user!") != std::string::npos);

  // Verify color codes for grey (thought) and white (assistant)
  // white: \033[37m
  // grey: \033[90m
  EXPECT_TRUE(output.find("\033[37m") != std::string::npos);
  EXPECT_TRUE(output.find("\033[90m") != std::string::npos);
}

TEST(UiTest, PrintAssistantMessageWithPrefix) {
  std::string content = "Hello world";
  std::string prefix = "  ";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

  PrintAssistantMessage(content, "", prefix);

  std::cout.rdbuf(old);
  std::string output = buffer.str();

  // No labels or bullets
  EXPECT_TRUE(output.find("Hello world") != std::string::npos);
}

TEST(UiTest, PrintAssistantMessageWithTokens) {
  std::string content = "Hello world";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

  PrintAssistantMessage(content, "", "", 123);

  std::cout.rdbuf(old);
  std::string output = buffer.str();

  EXPECT_TRUE(output.find("Hello world") != std::string::npos);
  EXPECT_TRUE(output.find("· 123 tokens") != std::string::npos);
  // Grey color code: \033[90m
  EXPECT_TRUE(output.find("\033[90m") != std::string::npos);
}

TEST(UiTest, PrintAssistantMessageWithTokensAndPrefix) {
  std::string content = "Hello world";
  std::string prefix = "| ";
  std::stringstream buffer;
  std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());

  PrintAssistantMessage(content, "", prefix, 123);

  std::cout.rdbuf(old);
  std::string output = buffer.str();

  // The body is indented by prefix + "    "
  // The tokens should be on a new line also indented by prefix + "    "
  EXPECT_TRUE(output.find("|     Hello world") != std::string::npos);
  EXPECT_TRUE(output.find("|     \033[90m· 123 tokens") != std::string::npos);
}

TEST(UiTest, WrapTextWithPrefix) {
    std::string text = "This is a long string that should be wrapped to multiple lines when the width is small.";
    std::string prefix = ">> ";
    std::string wrapped = WrapText(text, 20, prefix);
    
    std::vector<std::string> lines = absl::StrSplit(wrapped, '\n');
    for (const auto& line : lines) {
        if (!line.empty()) {
            EXPECT_TRUE(absl::StartsWith(line, prefix));
        }
    }
}

}  // namespace slop
