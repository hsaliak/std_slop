#ifndef SLOP_INTERFACE_RENDERER_H_
#define SLOP_INTERFACE_RENDERER_H_

#include <string>

#include "absl/base/no_destructor.h"

#include "markdown/parser.h"
#include "markdown/renderer.h"

namespace slop {

enum class RenderTarget {
  kTerminal,
  kPlainText,
};

class Renderer {
 public:
  static Renderer& Get();

  void RenderMarkdown(const std::string& markdown, const std::string& prefix, std::string* rendered,
                      RenderTarget target = RenderTarget::kTerminal);
  void PrintMarkdown(const std::string& markdown, const std::string& prefix = "",
                     RenderTarget target = RenderTarget::kTerminal);

 private:
  Renderer() = default;
  ~Renderer() = default;
  friend class absl::NoDestructor<Renderer>;

  markdown::MarkdownParser parser_;
  markdown::MarkdownRenderer renderer_;
};

}  // namespace slop

#endif  // SLOP_INTERFACE_RENDERER_H_
