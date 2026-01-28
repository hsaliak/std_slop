#ifndef MARKDOWN_PARSER_H_
#define MARKDOWN_PARSER_H_

#include <memory>
#include <string>
#include <vector>
#include <map>
#include "tree_sitter/api.h"
#include "absl/status/statusor.h"

namespace slop {
namespace markdown {

struct Range {
    uint32_t start_byte;
    uint32_t end_byte;
    
    bool operator<(const Range& other) const {
        if (start_byte != other.start_byte) return start_byte < other.start_byte;
        return end_byte < other.end_byte;
    }
};

struct Injection {
    std::string language;
    Range range;
    std::shared_ptr<TSTree> tree;
};

class ParsedMarkdown final {
public:
    ParsedMarkdown(std::string source, TSTree* tree);
    ~ParsedMarkdown();

    // Disable copy, enable move
    ParsedMarkdown(const ParsedMarkdown&) = delete;
    ParsedMarkdown& operator=(const ParsedMarkdown&) = delete;
    ParsedMarkdown(ParsedMarkdown&&) = default;
    ParsedMarkdown& operator=(ParsedMarkdown&&) = default;

    const std::string& source() const { return source_; }
    TSTree* tree() const { return tree_.get(); }
    
    void AddInjection(Injection injection);
    const Injection* GetInjection(Range range) const;

private:
    std::string source_;
    std::unique_ptr<TSTree, decltype(&ts_tree_delete)> tree_;
    std::map<Range, Injection> injections_;
};

class MarkdownParser final {
public:
    MarkdownParser();
    ~MarkdownParser();

    [[nodiscard]] absl::StatusOr<std::unique_ptr<ParsedMarkdown>> Parse(std::string source);

private:
    TSParser* parser_;
    TSParser* inline_parser_;
};

} // namespace markdown
} // namespace slop

#endif // MARKDOWN_PARSER_H_
