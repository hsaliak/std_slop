#include "markdown/renderer.h"

#include "interface/color.h"

#include <gtest/gtest.h>


namespace slop::markdown {

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
  EXPECT_NE(rendered.find('A'), std::string::npos);
  EXPECT_NE(rendered.find('1'), std::string::npos);
}

TEST(MarkdownRendererTest, LongTableWrapping) {
  MarkdownParser parser;
  // A table with a very long cell content
  auto p_res = parser.Parse(
      "| Header 1 | Header 2 |\n|---|---|\n| This is a very long cell content that should be wrapped if the width is "
      "small | Short |\n");
  ASSERT_TRUE(p_res.ok());

  MarkdownRenderer renderer;
  // Set a small max width to force wrapping
  renderer.SetMaxWidth(40);
  std::string rendered = renderer.Render(*p_res.value());

  // Check that the content is present and that it contains newlines within the table structure
  EXPECT_NE(rendered.find("Header 1"), std::string::npos);
  EXPECT_NE(rendered.find("very long"), std::string::npos);

  // Count the number of │ characters to see if we have multiple lines for the same row
  int pipe_count = 0;
  size_t pos = rendered.find("│");
  while (pos != std::string::npos) {
    pipe_count++;
    pos = rendered.find("│", pos + 1);
  }

  // A normal 2-column, 2-row table (header + 1 row) would have 2 * (2 * 2 + 1) = 10 pipes?
  // Wait, each row has 3 pipes (start, middle, end).
  // Header: 3 pipes.
  // Data row: 3 pipes * lines.
  // If it wraps into 3 lines, that's 9 pipes for the data row.
  // Total pipes should be > 6.
  EXPECT_GT(pipe_count, 6);
}

TEST(MarkdownRendererTest, TableWrappingEdgeCases) {
  MarkdownParser parser;
  MarkdownRenderer renderer;

  // 1. Extremely narrow width
  // Use a blank line before to ensure table parsing
  auto p_res1 = parser.Parse("\n| One | Two |\n|---|---|\n| Content | More |\n");
  ASSERT_TRUE(p_res1.ok());
  renderer.SetMaxWidth(10);
  std::string rendered1 = renderer.Render(*p_res1.value());
  // "Content" might be wrapped, so check for a prefix
  EXPECT_NE(rendered1.find("Cont"), std::string::npos);

  // 2. Multi-byte chars (Emoji)
  auto p_res2 = parser.Parse("\n| Emoji | Text |\n|---|---|\n| 🚀 | Rocket |\n");
  ASSERT_TRUE(p_res2.ok());
  renderer.SetMaxWidth(20);
  std::string rendered2 = renderer.Render(*p_res2.value());
  EXPECT_NE(rendered2.find("🚀"), std::string::npos);

  // 3. ANSI styling in cells
  auto p_res3 = parser.Parse("\n| Styled | Plain |\n|---|---|\n| **Bold** | Normal |\n");
  ASSERT_TRUE(p_res3.ok());
  renderer.SetMaxWidth(40);
  std::string rendered3 = renderer.Render(*p_res3.value());
  // The injection should have happened, so we expect Bold escape codes
  EXPECT_NE(rendered3.find(ansi::theme::markdown::Bold), std::string::npos);
  EXPECT_NE(rendered3.find("Bold"), std::string::npos);
}

} // namespace slop::markdown

