# Semantic-Aware Tooling with Tree-sitter

This document outlines the proposed integration of Tree-sitter into the tooling stack to enable structural understanding of the codebase.

## Proposed Tools

### 1. Semantic Grep & Structural Search (`tools.semantic_grep`)
- **Concept**: Use Tree-sitter's S-expression query language to find specific code patterns.
- **Benefit**: Eliminates false positives from comments, strings, or similarly named variables in different scopes.

### 2. Structural Summarization (`tools.get_skeleton`)
- **Concept**: Extract the "API surface" of a file by stripping out function/method bodies.
- **Benefit**: Allows the Orchestrator to "map" large files using minimal tokens.

### 3. Logical Code Chunking (`tools.chunk_by_symbol`)
- **Concept**: Chunk by logical boundaries (top-level definitions) instead of line counts.
- **Benefit**: Ensures functions/classes are analyzed within their complete logical context.

### 4. Scope-Aware Symbol Renaming (`tools.semantic_rename`)
- **Concept**: Identify all occurrences of a symbol within its actual scope.
- **Benefit**: Enables safe, automated refactoring that respects variable shadowing.

### 5. Import/Dependency Analysis (`tools.get_dependencies`)
- **Concept**: Parse import/include statements to build a local dependency graph.
- **Benefit**: Automatically identifies affected files when a change is made.

### 6. Semantic Diffing
- **Concept**: Compare files based on AST changes rather than line changes.
- **Benefit**: Better tracking of moved code and structural transformations.

## Implementation Roadmap

1. **C++ Foundation**: Create a `TreeSitterTool` in `core/` to expose `ts_query`.
2. **Tooling Integration**: Add higher-level wrappers for agent-facing usage.
3. **Integration**: Update the `planner` skill to utilize these tools for discovery and refactoring.

