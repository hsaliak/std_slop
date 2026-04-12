#include "interface/renderer.h"

#include <iostream>

#include "interface/color.h"
#include "interface/terminal.h"  // For GetTerminalWidth and VisibleLength

namespace slop {

Renderer& Renderer::Get() {
  static absl::NoDestructor<Renderer> instance;
  return *instance;
}

void Renderer::RenderMarkdown(const std::string& markdown, const std::string& prefix, std::string* rendered,
                              RenderTarget target) {
  auto parsed_or = parser_.Parse(markdown);
  if (!parsed_or.ok()) {
    *rendered = prefix + markdown + "\n";
    return;
  }

  size_t width = GetTerminalWidth();
  size_t prefix_len = VisibleLength(prefix);
  renderer_.SetMaxWidth(width > prefix_len + 5 ? width - prefix_len : 0);
  renderer_.SetStyleMode(target == RenderTarget::kPlainText ? markdown::MarkdownRenderer::StyleMode::kPlainText
                                                             : markdown::MarkdownRenderer::StyleMode::kAnsi);
  renderer_.Render(**parsed_or, rendered);
}

void Renderer::PrintMarkdown(const std::string& markdown, const std::string& prefix, RenderTarget target) {
  std::string rendered;
  RenderMarkdown(markdown, prefix, &rendered, target);
  std::cout << rendered;
  std::cout << "\n";
}

}  // namespace slop
