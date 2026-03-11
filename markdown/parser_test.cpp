#include "markdown/parser.h"

#include <string>
#include <string_view>

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

TEST(MarkdownParserTest, DiffFenceCreatesUnifiedDiffInjection) {
  constexpr std::string_view kMarkdown =
      "```diff\n"
      "--- a/file.txt\n"
      "+++ b/file.txt\n"
      "@@ -1 +1 @@\n"
      "-old\n"
      "+new\n"
      "```\n";

  MarkdownParser parser;
  auto result = parser.Parse(std::string(kMarkdown));
  ASSERT_TRUE(result.ok());

  auto parsed = std::move(result.value());
  const size_t content_start = kMarkdown.find("--- a/file.txt");
  ASSERT_NE(content_start, std::string_view::npos);
  const size_t content_end = kMarkdown.rfind("\n```");
  ASSERT_NE(content_end, std::string_view::npos);

  const Injection* injection = parsed->GetInjection({static_cast<uint32_t>(content_start),
                                                     static_cast<uint32_t>(content_end + 1)});
  ASSERT_NE(injection, nullptr);
  EXPECT_EQ(injection->language, "diff");
  EXPECT_NE(injection->tree, nullptr);
}

}  // namespace slop::markdown
