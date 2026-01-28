# Markdown Parser & Renderer

A C++ library for parsing and rendering Markdown to terminal output with ANSI styling, powered by [Tree-sitter](https://tree-sitter.github.io/tree-sitter/).

## Architecture

The library is split into two main components:

### 1. Markdown Parser (`//markdown:parser`)
Wraps the `tree-sitter-markdown` and `tree-sitter-markdown-inline` grammars.
- **Dual-Phase Parsing**: First parses block-level structures, then automatically identifies and parses inline content (injections).
- **Injection Support**: Detects fenced code blocks and tracks their language and content ranges.
- **Efficiency**: Reuses parsers and uses optimized lookups for injected trees.

### 2. Markdown Renderer (`//markdown:renderer`)
Transforms a `ParsedMarkdown` object into an ANSI-styled `std::string`.
- **DFS Traversal**: Recursively visits nodes in the Tree-sitter forest.
- **Centralized Styling**: Uses `color.h` (specifically the `ansi::theme::markdown` namespace) for all visual definitions.
- **Bleed Prevention**: Automatically re-applies styles when traversing across markers (e.g., ensuring a heading text remains colored after the `#` marker reset).

## API Reference

### `MarkdownParser`
The primary entry point for parsing Markdown content.
- `MarkdownParser()`: Initializes the Tree-sitter parsers and languages.
- `absl::StatusOr<std::unique_ptr<ParsedMarkdown>> Parse(std::string source)`: Parses the provided Markdown source string. Returns a `ParsedMarkdown` object containing the syntax forest and detected injections.

### `ParsedMarkdown`
A container for the results of a parse operation.
- `const std::string& source() const`: Returns the original Markdown source string.
- `TSTree* tree() const`: Returns the root block-level Tree-sitter tree.
- `const Injection* GetInjection(Range range) const`: Retrieves an injection (e.g., inline formatting or code block) for a specific byte range, if one exists.

### `MarkdownRenderer`
Handles the conversion of a `ParsedMarkdown` object into a styled string.
- `void SetMaxWidth(size_t width)`: Sets the maximum width for layout (e.g., terminal width). If set to 0 (default), no wrapping or shrinking is applied.
- `std::string Render(const ParsedMarkdown& parsed)`: Performs a recursive DFS traversal of the syntax forest and applies ANSI styles defined in `color.h`. Returns a terminal-ready string.

#### Intelligent Table Wrapping
The renderer includes advanced table layout logic:
- **Greedy Column Shrinking**: If a table exceeds `max_width`, the renderer iteratively shrinks the widest columns until the table fits or a minimum column width is reached.
- **Multi-line Cells**: Content within table cells is word-wrapped to the column's assigned width, creating multi-line rows that preserve the table grid structure.
- **UTF-8 Awareness**: Width calculations and word-breaking logic are multi-byte aware, ensuring correct rendering of international characters and ANSI-styled content.

### Supporting Types
- `Range`: Represents a byte range `[start_byte, end_byte)` in the source document.
- `Injection`: Contains metadata about a sub-language injection, including the language name, the range it occupies, and a pointer to its own `TSTree`.

## Usage

```cpp
#include "markdown/parser.h"
#include "markdown/renderer.h"

// 1. Create a parser and parse source
slop::markdown::MarkdownParser parser;
auto result = parser.Parse("# Hello World\nThis is **bold**.");

if (result.ok()) {
    auto& parsed = *result.value();
    
    // 2. Render to terminal-ready string
    slop::markdown::MarkdownRenderer renderer;
    renderer.SetMaxWidth(80); // Optional: Set layout boundary
    std::string terminal_output = renderer.Render(parsed);
    
    std::cout << terminal_output << std::endl;
}
```

## Styling

Styles are centralized in `color.h`. You can customize the look of headers, code blocks, and other elements by modifying the `ansi::theme::markdown` namespace.

## Performance Considerations
- **Allocations**: The renderer pre-allocates the output string buffer based on input size.
- **Lookups**: Injected trees are stored in a `std::map` with custom `Range` keys for $O(\log N)$ lookup during rendering.
- **Tree-sitter Forest**: The library manages multiple `TSTree` objects efficiently via `shared_ptr` and custom destructors.
