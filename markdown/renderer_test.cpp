#include "markdown/renderer.h"

#include "interface/color.h"

#include <gtest/gtest.h>

namespace slop {
namespace markdown {

TEST(MarkdownRendererTest, BasicRendering) {
  MarkdownParser parser;
  auto p_res = parser.Parse("# Title\n");
  ASSERT_TRUE(p_res.ok());

  MarkdownRenderer renderer;
  std::string rendered = renderer.Render(*p_res.value());

  EXPECT_NE(rendered.find(ansi::theme::markdown::Header), std::string::npos);
  EXPECT_NE(rendered.find("Title"), std::string::npos);
}

TEST(MarkdownRendererTest, NestedStyles) {
  MarkdownParser parser;
  auto p_res = parser.Parse("**bold *italic***\n");
  ASSERT_TRUE(p_res.ok());

  MarkdownRenderer renderer;
  std::string rendered = renderer.Render(*p_res.value());

  EXPECT_NE(rendered.find(ansi::theme::markdown::Bold), std::string::npos);
  EXPECT_NE(rendered.find(ansi::theme::markdown::Italic), std::string::npos);
}

TEST(MarkdownRendererTest, CodeBlock) {
  MarkdownParser parser;
  auto p_res = parser.Parse("```cpp\nint main() {}\n```\n");
  ASSERT_TRUE(p_res.ok());

  MarkdownRenderer renderer;
  std::string rendered = renderer.Render(*p_res.value());

  EXPECT_NE(rendered.find(ansi::theme::markdown::CodeBlock), std::string::npos);
  EXPECT_NE(rendered.find("int main()"), std::string::npos);
}

TEST(MarkdownRendererTest, LinkRendering) {
  MarkdownParser parser;
  auto p_res = parser.Parse("[Google](https://google.com)\n");
  ASSERT_TRUE(p_res.ok());

  MarkdownRenderer renderer;
  std::string rendered = renderer.Render(*p_res.value());

  EXPECT_NE(rendered.find(ansi::theme::markdown::LinkText), std::string::npos);
  EXPECT_NE(rendered.find(ansi::theme::markdown::LinkUrl), std::string::npos);
}

TEST(MarkdownRendererTest, ListRendering) {
  MarkdownParser parser;
  auto p_res = parser.Parse("- Item 1\n- Item 2\n");
  ASSERT_TRUE(p_res.ok());

  MarkdownRenderer renderer;
  std::string rendered = renderer.Render(*p_res.value());

  EXPECT_NE(rendered.find(ansi::theme::markdown::ListMarker), std::string::npos);
}

TEST(MarkdownRendererTest, BlockQuote) {
  MarkdownParser parser;
  auto p_res = parser.Parse("> This is a quote\n");
  ASSERT_TRUE(p_res.ok());

  MarkdownRenderer renderer;
  std::string rendered = renderer.Render(*p_res.value());

  EXPECT_NE(rendered.find(ansi::theme::markdown::Quote), std::string::npos);
  EXPECT_NE(rendered.find("quote"), std::string::npos);
}

TEST(MarkdownRendererTest, HorizontalRule) {
  MarkdownParser parser;
  auto p_res = parser.Parse("---\n");
  ASSERT_TRUE(p_res.ok());

  MarkdownRenderer renderer;
  std::string rendered = renderer.Render(*p_res.value());

  EXPECT_NE(rendered.find(ansi::theme::markdown::HorizontalRule), std::string::npos);
}

TEST(MarkdownRendererTest, MultiLanguageInjections) {
  MarkdownParser parser;
  // Test that multiple injections (inline + code block) coexist
  auto p_res = parser.Parse("# Title\n**bold**\n```cpp\nint x = 0;\n```\n");
  ASSERT_TRUE(p_res.ok());

  MarkdownRenderer renderer;
  std::string rendered = renderer.Render(*p_res.value());

  EXPECT_NE(rendered.find(ansi::theme::markdown::Header), std::string::npos);
  EXPECT_NE(rendered.find(ansi::theme::markdown::Bold), std::string::npos);
  EXPECT_NE(rendered.find(ansi::theme::markdown::CodeBlock), std::string::npos);
  EXPECT_NE(rendered.find("int x = 0;"), std::string::npos);
}

TEST(MarkdownRendererTest, TableRendering) {
  MarkdownParser parser;
  auto p_res = parser.Parse("| A | B |\n|---|---|\n| 1 | 2 |\n");
  ASSERT_TRUE(p_res.ok());

  MarkdownRenderer renderer;
  std::string rendered = renderer.Render(*p_res.value());

  EXPECT_NE(rendered.find("┌"), std::string::npos);
  EXPECT_NE(rendered.find("┤"), std::string::npos);
  EXPECT_NE(rendered.find("└"), std::string::npos);
  EXPECT_NE(rendered.find("A"), std::string::npos);
  EXPECT_NE(rendered.find("1"), std::string::npos);
}

}  // namespace markdown
}  // namespace slop
