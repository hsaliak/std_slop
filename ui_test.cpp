#include "ui.h"
#include <gtest/gtest.h>
#include <iostream>
#include <sstream>

namespace sentinel {

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

} // namespace sentinel
