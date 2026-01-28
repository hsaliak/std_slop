#include <iostream>
#include <string>
#include "tree_sitter/api.h"
#include <gtest/gtest.h>

extern "C" {
const TSLanguage *tree_sitter_markdown(void);
const TSLanguage *tree_sitter_markdown_inline(void);
}

TEST(TreeSitterMarkdownTest, BasicParsing) {
    TSParser *parser = ts_parser_new();
    ASSERT_NE(parser, nullptr);

    const TSLanguage *language = tree_sitter_markdown();
    ASSERT_NE(language, nullptr);

    bool success = ts_parser_set_language(parser, language);
    ASSERT_TRUE(success);

    std::string source_code = "# Hello World\nThis is a test.";
    TSTree *tree = ts_parser_parse_string(
        parser,
        nullptr,
        source_code.c_str(),
        source_code.length()
    );
    ASSERT_NE(tree, nullptr);

    TSNode root_node = ts_tree_root_node(tree);
    ASSERT_FALSE(ts_node_is_null(root_node));
    
    // Check if it's a document
    const char* type = ts_node_type(root_node);
    EXPECT_STREQ(type, "document");

    ts_tree_delete(tree);
    ts_parser_delete(parser);
}

TEST(TreeSitterMarkdownTest, InlineParsing) {
    TSParser *parser = ts_parser_new();
    ASSERT_NE(parser, nullptr);

    const TSLanguage *language = tree_sitter_markdown_inline();
    ASSERT_NE(language, nullptr);

    bool success = ts_parser_set_language(parser, language);
    ASSERT_TRUE(success);

    std::string source_code = "**bold**";
    TSTree *tree = ts_parser_parse_string(
        parser,
        nullptr,
        source_code.c_str(),
        source_code.length()
    );
    ASSERT_NE(tree, nullptr);

    TSNode root_node = ts_tree_root_node(tree);
    ASSERT_FALSE(ts_node_is_null(root_node));

    ts_tree_delete(tree);
    ts_parser_delete(parser);
}
