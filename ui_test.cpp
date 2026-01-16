#include "ui.h"
#include <gtest/gtest.h>
#include <iostream>
#include <sstream>
#include <cstdlib>

namespace slop {

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

TEST(UiTest, FormatAssembledContextOpenAI) {
    std::string json = R"({
        "messages": [
            {"role": "system", "content": "You are a helper."},
            {"role": "user", "content": "Hello!"}
        ]
    })";
    std::string formatted = FormatAssembledContext(json);
    EXPECT_TRUE(formatted.find("[SYSTEM]") != std::string::npos);
    EXPECT_TRUE(formatted.find("You are a helper.") != std::string::npos);
    EXPECT_TRUE(formatted.find("[USER]") != std::string::npos);
    EXPECT_TRUE(formatted.find("Hello!") != std::string::npos);
}

TEST(UiTest, SmartDisplayFallback) {
    // Force EDITOR to be empty
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

} // namespace slop
