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
- `void Render(const ParsedMarkdown& parsed, std::string* output)`: **(Preferred)** Appends the rendered output to the provided `std::string`. This "sink" model allows for efficient pre-allocation and avoids large string copies when building complex UI components.
- `std::string Render(const ParsedMarkdown& parsed)`: Convenience wrapper that returns a new string.

#### Syntax Highlighting

The renderer uses Tree-sitter grammars to provide syntax highlighting within fenced code blocks. Currently supported languages include:

- **C / C++** (`cpp`, `c`, `cc`, `cxx`, `hpp`)
- **Python** (`python`, `py`)
- **Go** (`go`)
- **JavaScript** (`javascript`, `js`)
- **Rust** (`rust`, `rs`)
- **Bash / Shell** (`bash`, `sh`)

### Intelligent Table Wrapping
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

// 1. Create a parser and parse source.
// The parser takes a std::string by value and moves it internally
// to ensure the buffer remains stable for the syntax tree.
slop::markdown::MarkdownParser parser;
std::string source = "# Hello World\nThis is **bold**.";
auto result = parser.Parse(std::move(source));

if (result.ok()) {
    auto& parsed = *result.value();
    
    // 2. Render to terminal-ready string.
    slop::markdown::MarkdownRenderer renderer;
    renderer.SetMaxWidth(80); 
    
    // Use the sink-based Render for better performance
    std::string terminal_output;
    terminal_output.reserve(parsed.source().length() * 2); // Heuristic for ANSI overhead
    renderer.Render(parsed, &terminal_output);
    
    std::cout << terminal_output << std::endl;
}
```

## Styling

Styles are centralized in `color.h`. You can customize the look of headers, code blocks, and other elements by modifying the `ansi::theme::markdown` namespace.

## Design & Performance Considerations

### Memory Management & Ownership
- **Parser Input**: `MarkdownParser::Parse` takes `std::string` by value. This is a deliberate design choice:
    - It allows the caller to `std::move` an existing buffer, avoiding any copies.
    - It guarantees that the `ParsedMarkdown` object owns its source buffer. This is critical because the underlying `tree-sitter` syntax tree maintains raw pointers into this buffer.
- **Renderer "Sink" Model**: The `Render(parsed, &output)` method is the primary way to generate output. By passing a pointer to an existing string, callers can reuse buffers or pre-allocate memory (via `reserve()`), which is significantly faster than returning large strings from deep recursion.

### Efficiency
- **Tree-sitter Forest**: The library manages multiple `TSTree` objects (main tree + injections) efficiently via `shared_ptr` and custom destructors.
- **Lookups**: Injected trees are stored in a `std::map` with custom `Range` keys for $O(\log N)$ lookup during rendering. This ensures that even with hundreds of injections (e.g., in a large table), finding the correct tree for a node is fast.
- **ANSI Styling**: The renderer minimizes style code bloat by only emitting ANSI reset codes when necessary and tracking the current styling state to avoid redundant escape sequences.
