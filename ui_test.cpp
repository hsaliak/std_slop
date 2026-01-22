#include "ui.h"
#include <gtest/gtest.h>
#include <iostream>
#include <sstream>
#include <cstdlib>

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
    EXPECT_TRUE(formatted.find("[SYSTEM INSTRUCTION]") != std::string::npos);
    EXPECT_TRUE(formatted.find("You are a helper.") != std::string::npos);
    EXPECT_TRUE(formatted.find("[user]") != std::string::npos);
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
    EXPECT_TRUE(formatted.find("[SYSTEM INSTRUCTION]") != std::string::npos);
    EXPECT_TRUE(formatted.find("GCA system prompt.") != std::string::npos);
    EXPECT_TRUE(formatted.find("[user]") != std::string::npos);
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
    EXPECT_TRUE(formatted.find("[user]") != std::string::npos);
    EXPECT_TRUE(formatted.find("OpenAI User") != std::string::npos);
    EXPECT_TRUE(formatted.find("[assistant]") != std::string::npos);
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



TEST(UiTest, FormatLinePadding) {
    std::string text = "Short";
    std::string formatted = FormatLine(text, "\033[44m", 20);
    // visible length of formatted (excluding ANSI) should be 20
    // Actually FormatLine returns Colorized text.
    // We expect \033[44m + \033[37m + "Short               " + \033[0m
    EXPECT_EQ(formatted, std::string("\033[44m\033[37mShort               \033[0m"));
}

TEST(UiTest, FormatLineTruncation) {
    std::string text = "This is a very long line that needs truncation";
    std::string formatted = FormatLine(text, "\033[44m", 10);
    // Expected: \033[44m\033[37mThis is...\033[0m
    // Wait, "This is..." is 10 chars.
    EXPECT_EQ(formatted, std::string("\033[44m\033[37mThis is...\033[0m"));
}

TEST(UiTest, GetTerminalWidth) {
    size_t width = GetTerminalWidth();
    EXPECT_GT(width, 0);
}

} // namespace slop
