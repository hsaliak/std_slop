#include "markdown/parser.h"

#include <gtest/gtest.h>

namespace slop::markdown {

TEST(MarkdownParserTest, BasicParse) {
  MarkdownParser parser;
  auto result = parser.Parse("# Hello\nWorld\n");
  ASSERT_TRUE(result.ok());

  auto parsed = std::move(result.value());
  EXPECT_EQ(parsed->source(), "# Hello\nWorld\n");
  ASSERT_NE(parsed->tree(), nullptr);

  TSNode root = ts_tree_root_node(parsed->tree());
  EXPECT_STREQ(ts_node_type(root), "document");
}

TEST(MarkdownParserTest, Injections) {
  MarkdownParser parser;
  auto result = parser.Parse("Check out this **bold** text and `code`.\n");
  ASSERT_TRUE(result.ok());

  auto parsed = std::move(result.value());
  // We expect an injection for the 'inline' node at [0-40]
  EXPECT_NE(parsed->GetInjection({0, 40}), nullptr);
}

TEST(MarkdownParserTest, EmptyInput) {
  MarkdownParser parser;
  auto result = parser.Parse("");
  ASSERT_TRUE(result.ok());
  auto parsed = std::move(result.value());
  EXPECT_EQ(parsed->source(), "");
}

}  // namespace slop::markdown
