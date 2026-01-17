# std::slop User Guide

## Overview
`std::slop` is a high-performance LLM CLI built for developers who want a SQL-backed, persistent conversation history with built-in tools for codebase exploration and context management.

## Installation
Build using Bazel:
```bash
bazel build //:std_slop
```

## Slash Commands

### Prompt-Ledger Mode
Enable Git-based tracking of all changes made by the LLM. When enabled, `[pl]` will appear in your prompt.
- `/prompt-ledger [on|off]`: Toggle Git-based prompt tracking. Prompts to `git init` if the directory is not a repository.
- `/prompt-ledger <GID>`: Show the diff of changes made during a specific interaction group.
- `/prompt-ledger show <GID>`: Alias for above.
- `/prompt-ledger`: Show current status and usage.

...
