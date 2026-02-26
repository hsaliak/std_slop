#ifndef SLOP_INTERFACE_RENDERER_H_
#define SLOP_INTERFACE_RENDERER_H_

#include <string>
#include "absl/base/no_destructor.h"
#include "markdown/parser.h"
#include "markdown/renderer.h"

namespace slop {

class Renderer {
 public:
  static Renderer& Get();

  void RenderMarkdown(const std::string& markdown, const std::string& prefix, std::string* rendered);
  void PrintMarkdown(const std::string& markdown, const std::string& prefix = "");

 private:
  Renderer() = default;
  ~Renderer() = default;
  friend class absl::NoDestructor<Renderer>;

  markdown::MarkdownParser parser_;
  markdown::MarkdownRenderer renderer_;
};

}  // namespace slop

#endif  // SLOP_INTERFACE_RENDERER_H_
