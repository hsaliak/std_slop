#include "markdown/parser.h"

#include <iostream>

extern "C" {
const TSLanguage* tree_sitter_markdown(void);
const TSLanguage* tree_sitter_markdown_inline(void);
}


namespace slop::markdown {

ParsedMarkdown::ParsedMarkdown(std::string source, TSTree* tree)
    : source_(std::move(source)), tree_(tree, ts_tree_delete) {}

ParsedMarkdown::~ParsedMarkdown() = default;

void ParsedMarkdown::AddInjection(Injection injection) {
  Range r = injection.range;
  injections_.emplace(r, std::move(injection));
}

const Injection* ParsedMarkdown::GetInjection(Range range) const {
  auto it = injections_.find(range);
  if (it != injections_.end()) return &it->second;
  return nullptr;
}

class InjectionFinder {
 public:
  InjectionFinder(TSParser* inline_parser, ParsedMarkdown* parsed) : inline_parser_(inline_parser), parsed_(parsed) {}

  void Find(TSNode node) {
    std::string_view type = ts_node_type(node);
    if (type == "inline" || type == "pipe_table_cell") {
      HandleInline(node);
    } else if (type == "fenced_code_block") {
      HandleCodeBlock(node);
    }

    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
      Find(ts_node_child(node, i));
    }
  }

 private:
  void HandleInline(TSNode node) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (end <= start || end > parsed_->source().length()) return;

    const std::string& source = parsed_->source();
    TSTree* tree = ts_parser_parse_string(inline_parser_, nullptr, source.c_str() + start, end - start);

    if (tree) {
      parsed_->AddInjection({"markdown_inline", {start, end}, std::shared_ptr<TSTree>(tree, ts_tree_delete)});
    }
  }

  void HandleCodeBlock(TSNode node) {
    uint32_t count = ts_node_child_count(node);
    std::string lang = "text";
    TSNode content_node = {};

    for (uint32_t i = 0; i < count; ++i) {
      TSNode child = ts_node_child(node, i);
      std::string_view child_type = ts_node_type(child);
      if (child_type == "info_string") {
        uint32_t s = ts_node_start_byte(child);
        uint32_t e = ts_node_end_byte(child);
        lang = parsed_->source().substr(s, e - s);
      } else if (child_type == "code_fence_content") {
        content_node = child;
      }
    }

    if (!ts_node_is_null(content_node)) {
      parsed_->AddInjection(
          {std::move(lang), {ts_node_start_byte(content_node), ts_node_end_byte(content_node)}, nullptr});
    }
  }

  TSParser* inline_parser_;
  ParsedMarkdown* parsed_;
};

MarkdownParser::MarkdownParser() {
  parser_ = ts_parser_new();
  ts_parser_set_language(parser_, tree_sitter_markdown());
  inline_parser_ = ts_parser_new();
  ts_parser_set_language(inline_parser_, tree_sitter_markdown_inline());
}

MarkdownParser::~MarkdownParser() {
  ts_parser_delete(parser_);
  ts_parser_delete(inline_parser_);
}

absl::StatusOr<std::unique_ptr<ParsedMarkdown>> MarkdownParser::Parse(std::string source) {
  TSTree* tree = ts_parser_parse_string(parser_, nullptr, source.c_str(), source.length());

  if (!tree) {
    return absl::InternalError("Failed to parse markdown");
  }

  auto parsed = std::make_unique<ParsedMarkdown>(std::move(source), tree);

  InjectionFinder finder(inline_parser_, parsed.get());
  finder.Find(ts_tree_root_node(parsed->tree()));

  return parsed;
}

} // namespace slop::markdown

