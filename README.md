# std::slop

`std::slop` is a persistent, SQLite-driven C++ CLI agent. It remembers your work through a per-project ledger, providing long-term recall, structured state management, and first-class Git integration.

## ✨ Key Features

- **📖 Ledger-Driven**: All interactions and tool calls are stored in SQLite for persistence and auditability.
- **🎛️ Context Control**: Fine-grained control over the conversation history via SQL-backed retrieval and rolling windows.
- **🛠️ Self-Managed State**: Autonomous updates to a task-specific `### STATE` and a markdown `Scratchpad` for complex planning.
- **🏷️ Memo System**: Tag-based knowledge persistence that survives across sessions.
- **🔍 Advanced Search**: `git_grep_tool` with boolean operators, multiple pathspecs, and smart truncation.
- **🤖 Multi-Model**: Supports Google Gemini and OpenAI-compatible APIs (OpenRouter, etc.).
- **🏗️ Hermetic Development**: Built with Bazel, including integrated linting and formatting.

## 🚀 Quick Start

### 📋 Prerequisites
- C++17 compiler (Clang/GCC)
- [Bazel](https://bazel.build/install) (Bazelisk recommended)
- **Git**: Targets must be valid git repositories.

### 🛠️ Build and Install
```bash
# Build the binary
bazel build //:std_slop

# Optional: Add to your PATH
cp ./bazel-bin/std_slop /usr/local/bin/
```

### ⌨️ Usage
`std::slop` works best when it can track a specific project. Initialize a git repository and run it from the root:
```bash
mkdir my-project && cd my-project
git init
std_slop
```

### ⚙️ Configuration
Set your API keys:
```bash
export GEMINI_API_KEY="your-key"
# OR
export OPENAI_API_KEY="your-key"
export OPENAI_API_BASE="https://openrouter.ai/api/v1"
```

## 💻 Code

- C++ Standard: C++17.
- Style: Google C++ Style Guide.
- Exceptions: Disabled (-fno-exceptions).
- Memory: RAII and std::unique_ptr exclusively.
- Error Handling: absl::Status and absl::StatusOr.
- Avoid threading and async primitives; if used, use absl-based primitives with std::thread. Any threading workflow requires tsan tests.
- Asan and Tsan clean at all times.

## 📚 Documentation

- **[User Guide](USERGUIDE.md)**: Detailed commands and workflow tips.
- **[Architecture & Schema](SCHEMA.md)**: Understanding the database-driven engine.
- **[Sessions](SESSIONS.md)**: How context isolation and management work.
- **[Walkthrough](WALKTHROUGH.md)**: A step-by-step example of using the agent.
- **[Contributing](CONTRIBUTING.md)**: Code style, formatting, and linting guidelines.

## 📁 Codebase Layout

- `core/`: Database management, orchestrators, and tool execution.
- `interface/`: Terminal UI, command handling, and line completion.
- `markdown/`: Tree-sitter based markdown rendering for the terminal.
- `main.cpp`: Entry point.
