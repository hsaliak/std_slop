# std::slop
```
    ____ _____ ____               ____  _     ___  ____  
   / ___|_   _|  _ \     _   _   / ___|| |   / _ \|  _ \ 
   \___ \ | | | | | |   (_) (_)  \___ \| |  | | | | |_) |
    ___) || | | |_| |    _   _   |___) | |__| |_| |  __/ 
   |____/ |_| |____/    (_) (_)  |____/|_____\___/|_|    
```
  

`std::slop` is a persistent, SQLite-driven C++ CLI agent. It remembers your work through a per-project ledger, providing long-term recall, structured state management, and first-class Git integration. It's goal is to make the context and it's use fully transparent and configurable.

## ✨ Key Features

- **📖 Ledger-Driven**: All interactions and tool calls are stored in SQLite for persistence and auditability.
- **🎛️ Context Control**: Fine-grained control over the conversation history via SQL-backed retrieval and rolling windows.
- **🛠️ Self-Managed State**: Autonomous updates to a task-specific `### STATE` and a markdown `Scratchpad` for complex planning.
- **🏷️ Memo System**: Tag-based knowledge persistence that survives across sessions. Think of these as your project's long term memory.
- **🔍 Advanced Search**: `git_grep_tool` with boolean operators, multiple pathspecs, and smart truncation.
- **⚡ Parallel Execution**: Executes multiple tool calls in parallel with result ordering and UI-thread safety.
- **📬 [Mail Mode](docs/mail_model.md)**: A patch-based iteration workflow for complex features. Patches are prepared on a staging branch, reviewed as atomic units, and only finalized after approval. 
- **🤖 Multi-Model**: Supports Google Gemini and OpenAI-compatible APIs (OpenRouter, etc.).

## 🚀 Quick Start

### 📋 Prerequisites
- C++17 compiler (Clang/GCC)
- [Bazel](https://bazel.build/install) (Bazelisk recommended)
- **Git**: Targets must be valid git repositories. Usually, a git add and an initial commit is sufficient to trigger all the git enabled features.

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

For quick one-off tasks, you can use **Batch Mode**:
```bash
std_slop --prompt "Refactor main.cpp to remove all unused includes" 
```
Batch mode also takes in `--model` which is useful to specify the model to use and `--session` which is useful to indicate the session the prompt should be executed under.
`/commands` are also supported. 

This is a good way to make `std::slop` act as a sub agent.

Read the [User Guide](USERGUIDE.md) for a detailed understanding of how to use std_slop, or [Walkthrough](WALKTHROUGH.md) to start with something simple.

### ⚙️ Configuration
Set your API keys:
```bash
export GEMINI_API_KEY="your-key"
# OR
export OPENAI_API_KEY="your-key"
export OPENAI_API_BASE="https://openrouter.ai/api/v1"
```

You can also use Google OAuth login.

#### Optional Environment Variables:
- `SLOP_DEBUG_HTTP=1`: Enable full verbose logging of all HTTP traffic (headers & bodies).

## 💻 Code

- C++ Standard: C++17.
- Style: Google C++ Style Guide.
- Exceptions: Disabled (-fno-exceptions).
- Memory: RAII and std::unique_ptr exclusively.
- Error Handling: absl::Status and absl::StatusOr.
- Concurrency: Parallel tool execution uses `std::thread` and `absl` synchronization primitives (`absl::Mutex`, `absl::Notification`). Thread safety is enforced via Absl thread-safety annotations (`ABSL_GUARDED_BY`) and verified with TSAN tests.
- Asan and Tsan clean at all times.

## 📚 Documentation

- **[User Guide](USERGUIDE.md)**: Detailed commands and workflow tips.
- **[Architecture & Schema](SCHEMA.md)**: Understanding the database-driven engine.
- **[Sessions](SESSIONS.md)**: How context isolation and management work.
- **[Context Management](CONTEXT_MANAGEMENT.md)**: The evolutionary history and strategy for managing model memory.
- **[Walkthrough](WALKTHROUGH.md)**: A step-by-step example of using the agent.
- **[Contributing](CONTRIBUTING.md)**: Code style, formatting, and linting guidelines.
- **[Context management](CONTEXT_MANAGEMENT.md)**: Details on how context is put together every turn, and how it's evolved.

## 🏗️ Architecture & Codebase Layout

### `core/` - The Engine
The core logic is divided into specialized modules:

- **`database.h`**: Manages the SQLite-backed ledger. Handles persistence for messages, memos, tools, and skills.
- **`tool_dispatcher.h`**: Implements a thread-safe parallel execution engine. It dispatches multiple tool calls concurrently while ensuring results are returned in the correct order for the LLM.
- **`cancellation.h`**: Provides a unified mechanism for interrupting tasks. It supports registering callbacks to kill shell processes or abort HTTP requests.
- **`orchestrator.h`**: High-level interface for model interaction. Implementations for Gemini and OpenAI manage history windowing and response parsing.
- **`shell_util.h`**: Executes shell commands in a separate process group, with support for real-time output polling and clean termination on cancellation.
- **`http_client.h`**: A minimalist, cancellation-aware HTTP client used for all model API calls.

### Interface & Display
- **`interface/`**: Implements the terminal UI. The UI is minimal but pleasing, uses readline for user input, color codes and ASCII Codes.
- **`markdown/`**: Uses `tree-sitter-markdown` to provide syntax highlighting (C++, Python, Go, JS, Rust, Bash) and structured rendering for agent responses. This is a stand alone Markdown  parser / renderer library in C++.
- **`main.cpp`**: The primary event loop. Coordinates between the Orchestrator, ToolDispatcher, and UI.
