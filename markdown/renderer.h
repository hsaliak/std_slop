#ifndef MARKDOWN_RENDERER_H_
#define MARKDOWN_RENDERER_H_

#include <string>

#include "markdown/parser.h"

namespace slop {
namespace markdown {

class MarkdownRenderer final {
 public:
  MarkdownRenderer() = default;

  // Sets the maximum width for rendering (e.g., terminal width).
  // If set to 0 (default), no wrapping/truncation is applied.
  void SetMaxWidth(size_t width) { max_width_ = width; }

  // Renders the parsed markdown to an ANSI-styled string
  [[nodiscard]] std::string Render(const ParsedMarkdown& parsed);

 public:
  struct TableColumn {
    size_t width = 0;
    enum Alignment { LEFT, CENTER, RIGHT } alignment = LEFT;
  };

 private:
  size_t max_width_ = 0;

  void RenderNodeRecursive(TSNode node, const ParsedMarkdown& parsed, std::string_view current_source,
                           std::string& output, int depth, TSTree* current_tree);

  void RenderTable(TSNode node, const ParsedMarkdown& parsed, std::string_view current_source, std::string& output,
                   int depth, TSTree* current_tree);
  std::string RenderCellToText(TSNode node, const ParsedMarkdown& parsed, std::string_view current_source,
                               TSTree* current_tree);
};

}  // namespace markdown
}  // namespace slop

#endif  // MARKDOWN_RENDERER_H_
