# std::slop
  
![std::slop](docs/slop.png)

`std::slop` is a persistent, SQLite-driven C++ CLI agent. It remembers your work through a per-project ledger, providing long-term recall, structured state management, and Built-in Git integration. It's goal is to make the context and it's use fully transparent and configurable.

## ✨ Key Features

- **📖 Ledger-Driven**: All interactions and tool calls are stored in SQLite for persistence and auditability.
- **🎛️ Context Control**: Fine-grained control over the conversation history via SQL-backed retrieval and rolling windows.
- **🛠️ Self-Managed State**: Autonomous updates to a task-specific `### STATE` and a markdown `Scratchpad` for complex planning.
- **🏷️ Memo System**: Tag-based knowledge persistence that survives across sessions. Think of these as your project's long term memory.
- **📜 Lua Control Plane**: Programmatic orchestration via a Lua 5.4 bridge, allowing complex scripts, safe staging, and parallel execution.
- **🔍 Boolean Search**: `git_grep_tool` with boolean operators, multiple pathspecs, and smart truncation.
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

Read the [User Guide](docs/USERGUIDE.md) for a detailed understanding of how to use std_slop, or [Walkthrough](docs/WALKTHROUGH.md) to start with something simple.

### ⚙️ Configuration
You can configure `std::slop` using environment variables or a configuration file.

#### Configuration File
The agent looks for a configuration file at `~/.config/slop/config.ini`. You can also specify a custom path using the `--config` flag.

```ini
[slop]
model = gemini-2.0-flash-exp
google_api_key = AIza...
# OR
openai_api_key = sk-...
openai_base_url = https://api.openai.com/v1
```

See [docs/example_config.ini](docs/example_config.ini) for a full list of options.

#### Environment Variables
- `GEMINI_API_KEY`: Google API key.
- `OPENAI_API_KEY`: OpenAI-compatible API key.
- `OPENAI_API_BASE`: Base URL for OpenAI-compatible providers.
- `SLOP_DEBUG_HTTP=1`: Enable full verbose logging of all HTTP traffic (headers & bodies).

You can also use Google OAuth login if no keys are provided.

## 💻 Code

- C++ Standard: C++17.
- Style: Google C++ Style Guide.
- Exceptions: Disabled (-fno-exceptions).
- Memory: RAII and std::unique_ptr exclusively.
- Error Handling: absl::Status and absl::StatusOr.
- Concurrency: Parallel tool execution is managed through the Lua control plane using `_async` variants and a job-based wait system. This allows for fine-grained control over concurrent operations. (`absl::Mutex`, `absl::Notification`). Thread safety is enforced via Absl thread-safety annotations (`ABSL_GUARDED_BY`) and verified with TSAN tests.
- Asan and Tsan clean at all times.

## 📚 Documentation

- **[User Guide](docs/USERGUIDE.md)**: Detailed commands and workflow tips.
- **[Architecture & Schema](docs/SCHEMA.md)**: Understanding the database-driven engine.
- **[Sessions](docs/SESSIONS.md)**: How context isolation and management work.
- **[Context Management](docs/CONTEXT_MANAGEMENT.md)**: The evolutionary history and strategy for managing model memory.
- **[Walkthrough](docs/WALKTHROUGH.md)**: A step-by-step example of using the agent.
- **[Lua Integration](docs/lua_integration.md)**: High-level orchestration and task safety via the Lua bridge.
- **[Contributing](docs/CONTRIBUTING.md)**: Code style, formatting, and linting guidelines.

## 🏗️ Architecture & Codebase Layout

### `core/` - The Engine
The core logic is divided into specialized modules:

- **`database.h`**: Manages the SQLite-backed ledger. Handles persistence for messages, memos, tools, and skills.
- **`tool_dispatcher.h`**: Implements a thread-safe parallel execution engine. It dispatches multiple tool calls concurrently while ensuring results are returned in the correct order for the LLM.
- **`cancellation.h`**: Provides a unified mechanism for interrupting tasks. It supports registering callbacks to kill shell processes or abort HTTP requests.
- **`orchestrator.h`**: High-level interface for model interaction. Implementations for Gemini and OpenAI manage history windowing and response parsing.
- **`shell_util.h`**: Executes shell commands in a separate process group, with support for real-time output polling and clean termination on cancellation.
- **`http_client.h`**: A minimalist, cancellation-aware HTTP client used for all model API calls.

### `lua-bridge/` - Orchestration Layer
- **`lua_bridge.h`**: Implements the Lua 5.4 environment. Provides the `run_lua` tool and manages the injection of global context (`tools`, `history`, `state`).
- **`preamble_lib.lua`**: The embedded standard library for the agent's Lua environment. Implements high-level helpers and the `slop_guard` safety mechanism.

### Interface & Display
- **`interface/`**: Implements the terminal UI. The UI is minimal but clean, uses readline for user input, color codes and ASCII Codes.
- **`markdown/`**: Uses `tree-sitter-markdown` to provide syntax highlighting (C++, Python, Go, JS, Rust, Bash) and structured rendering for agent responses. This is a stand alone Markdown  parser / renderer library in C++.
- **`main.cpp`**: The primary event loop. Coordinates between the Orchestrator, ToolDispatcher, and UI.
