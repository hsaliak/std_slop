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
TEST(IniParserTest, EnvironmentVariableExpansion) {
  setenv("TEST_VAR", "expanded_value", 1);
  std::string ini = "key = ${TEST_VAR}\nkey2 = $TEST_VAR";
  auto config = ParseIni(ini);
  EXPECT_EQ(config[""]["key"], "expanded_value");
  EXPECT_EQ(config[""]["key2"], "expanded_value");
  unsetenv("TEST_VAR");
}

TEST(IniParserTest, MissingEnvironmentVariable) {
  // Ensure the variable is NOT set
  unsetenv("NON_EXISTENT_VAR");
  std::string ini = "key = ${NON_EXISTENT_VAR}";
  auto config = ParseIni(ini);
  // Current implementation expands missing vars to empty strings
  EXPECT_EQ(config[""]["key"], "");
}

TEST(IniParserTest, PreservesLiteralJsonValuesWhenExpansionIsDisabled) {
  setenv("MCP_LITERAL_TEST", "expanded", 1);
  const IniConfig config = ParseIni("[server]\nargs_json = [\"$MCP_LITERAL_TEST #; value\"]\n", false);
  EXPECT_EQ(config.at("server").at("args_json"), "[\"$MCP_LITERAL_TEST #; value\"]");
  unsetenv("MCP_LITERAL_TEST");
}

}  // namespace slop
