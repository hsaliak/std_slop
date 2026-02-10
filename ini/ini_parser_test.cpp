#include "ini/ini_parser.h"
#include <gtest/gtest.h>

namespace slop {
namespace {

TEST(IniParserTest, BasicParsing) {
  std::string_view content = R"(
[slop]
model = gemini-pro
db = slop.db
strip_reasoning = true

[other]
key = value
)";
  
  IniConfig config = ParseIni(content);
  
  EXPECT_EQ(config["slop"]["model"], "gemini-pro");
  EXPECT_EQ(config["slop"]["db"], "slop.db");
  EXPECT_EQ(config["slop"]["strip_reasoning"], "true");
  EXPECT_EQ(config["other"]["key"], "value");
}

TEST(IniParserTest, CommentsAndSpacing) {
  std::string_view content = R"(
# Global comment
[slop]
  model  =  gemini-pro  # end of line comment
; another comment
  db = slop.db
)";
  
  IniConfig config = ParseIni(content);
  
  EXPECT_EQ(config["slop"]["model"], "gemini-pro");
  EXPECT_EQ(config["slop"]["db"], "slop.db");
}

TEST(IniParserTest, EmptyAndGlobal) {
  std::string_view content = "key=value\n[section]\nfoo=bar";
  IniConfig config = ParseIni(content);
  
  EXPECT_EQ(config[""]["key"], "value");
  EXPECT_EQ(config["section"]["foo"], "bar");
}

}  // namespace
}  // namespace slop
