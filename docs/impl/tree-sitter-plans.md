# Semantic-Aware Tooling with Tree-sitter

> Implementation/design-history note: this document is a proposal, not a current user-facing feature guide.

This document records a proposal for integrating Tree-sitter-based structural tooling into `std::slop`.

## Proposed Tooling Areas

1. Semantic grep / structural search
2. Structural summarization
3. Logical code chunking by symbol
4. Scope-aware symbol renaming
5. Import / dependency analysis
6. Semantic diffing

## Intended Benefits

- Fewer false positives than plain text search
- Better file mapping for large codebases
- Safer refactoring primitives
- More structure-aware code review and planning

## Historical Roadmap

1. Add a Tree-sitter-backed foundation in `core/`.
2. Expose agent-facing wrappers for structural queries.
3. Integrate those tools into discovery and refactoring flows.

This file remains as a design proposal/history document. If Tree-sitter tooling becomes active, a current user-facing guide should live outside `docs/impl/`.
