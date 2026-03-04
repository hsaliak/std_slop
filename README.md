# std::slop
  

[![CI/CD - Multi-Platform Build & Release](https://github.com/hsaliak/std_slop/actions/workflows/ci_cd.yml/badge.svg)](https://github.com/hsaliak/std_slop/actions/workflows/ci_cd.yml)

[![CodeQL Advanced](https://github.com/hsaliak/std_slop/actions/workflows/codeql.yml/badge.svg)](https://github.com/hsaliak/std_slop/actions/workflows/codeql.yml)

![std::slop](docs/slop.png)

`std::slop` is a persistent, SQLite-driven C++ CLI agent. It remembers your work through per-session ledgers, providing long-term recall, structured state management. std::slop features built-in Git integration. It's goal is to be an agent for which the context and its use fully transparent and configurable.

## ✨ Key Features

- **🎭 Personas & Skills**: Define global agent instructions via `AGENTS.md` and extend capabilities using modular, on-demand `SKILL.md` files.
- **📖 Ledger-Driven**: All interactions and tool calls are stored in SQLite for persistence and auditability. 
- **🎛️ Context Control**: Granular control over conversation history via SQL-backed retrieval and rolling windows. As the context is built per-session, you can create multiple sessions and even clone existing ones to go down different paths.
- **🏷️ Memo System**: Tag-based knowledge persistence that survives across sessions. Think of these as your project's long-term memory.
- **📜 [JavaScript Control Plane](docs/js_integration.md)**: Programmatic orchestration via a QuickJS bridge, allowing scripts, staging, and execution. std::slop uses this REPL to accomplish all it's tasks.
- **📬 [Mail Model](docs/mail_model_impl.md)**: A patch-based iteration workflow for complex features. Patches are prepared on a staging branch, reviewed as atomic units, and only finalized after approval.  Use this if you want a clean, bisect-safe view of changes, and want to be 'in the loop'. You can, of course offload the reviews to a `code_reviewer` skill as well.
- **🤖 Multi-Model**: Supports Google Gemini and OpenAI-compatible APIs (OpenRouter, etc.).
- **📣 Hotwords**: Quick, single-turn skill activation using `hey <skill> <query>` syntax. Eg: "hey code_reviewer review these patches".

## 🚀 Quick Start

### Download
The project ships Linux x86-64 and OSX binaries every [release](https://github.com/hsaliak/std_slop/releases). You can directly use them.

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
Batch mode also takes in `--model` which is useful to specify the model to use and `--session` which is useful to indicate the session the prompt should be executed under. Batch mode works off an in memory sqlite db. If you want the db persisted you can point it to a DB with the `--prompt-db` argument.
`/commands` are also supported. 


Read the [User Guide](docs/USERGUIDE.md) for a detailed understanding of how to use std_slop, or [Walkthrough](docs/WALKTHROUGH.md) to start with something simple.

### Authentication Quick Notes
- OpenAI OAuth (Responses API): `./slop_auth.sh chatgpt-plus` then run with `--openai_oauth`.

### ⚙️ Configuration
You can configure `std::slop` using environment variables or a configuration file.

#### Configuration File
The agent looks for a configuration file at `~/.config/slop/config.ini`. You can also specify a custom path using the `--config` flag.
It is STRONGLY RECOMMENDED that slop.db lies in a central directory or outside the codebase. It generates 2 other artifact files, at least ensure that
your .gitignore contains this. The context ledger is completely stored in the database, and it can inadvertently capture information from your environment if you are not careful. Eg Environment Variables.

```ini
[slop]
model = gemini-3-flash-preview
# OR
openai_api_key = sk-...
openai_base_url = https://api.openai.com/v1
# use_responses = true   # optional: use OpenAI Responses API with API key mode
# openai_oauth = true    # optional: use OpenAI OAuth token + Responses API
# openai_oauth_token_path = /custom/path/chatgpt_plus_token.json
```

See [docs/example_config.ini](docs/example_config.ini) for a full list of options.

#### Environment Variables
- `SLOP_DEBUG_HTTP=1`: Enable full verbose logging of all HTTP traffic (headers & bodies).


## 💻 Code

- C++ Standard: C++17.
- Style: Google C++ Style Guide.
- Exceptions: Disabled (-fno-exceptions).
- Memory: RAII and std::unique_ptr exclusively.
- Error Handling: absl::Status and absl::StatusOr.
- Concurrency: Parallel tool execution is managed through the JavaScript control plane using `_async` variants and a job-based wait system. This allows for granular control over concurrent operations. (`absl::Mutex`, `absl::Notification`). Thread safety is enforced via Absl thread-safety annotations (`ABSL_GUARDED_BY`) and verified with TSAN tests.
- Asan and Tsan clean at all times.

## 📚 Documentation

- **[Personas & Skills](docs/CONTEXT.md)**: Understanding global context injection and modular skills.
- **[User Guide](docs/USERGUIDE.md)**: Detailed commands and workflow tips.
- **[Architecture & Schema](docs/SCHEMA.md)**: Understanding the database-driven engine.
- **[Sessions](docs/SESSIONS.md)**: How context isolation and management work.
- **[Context Management](docs/CONTEXT_MANAGEMENT.md)**: The history and strategy for managing model memory.
- **[Walkthrough](docs/WALKTHROUGH.md)**: A step-by-step example of using the agent.
- **[JavaScript Integration](docs/js_integration.md)**: high-level orchestration and task safety via the QuickJS bridge.
- **[Contributing](docs/CONTRIBUTING.md)**: Code style, formatting, and linting guidelines.

## 🏗️ Architecture & Codebase Layout

### `core/` - The Engine
The core logic is divided into modules:

- **`database.h`**: Manages the SQLite-backed ledger. Handles persistence for messages, memos, tools, and skills.
- **`tool_dispatcher.h`**: Implements a thread-safe execution engine. It dispatches multiple tool calls concurrently while ensuring results are returned in the proper order for the LLM.
- **`cancellation.h`**: Provides a mechanism for interrupting tasks. It supports registering callbacks to kill shell processes or abort HTTP requests.
- **`orchestrator.h`**: high-level interface for model interaction. Implementations for Gemini and OpenAI manage history windowing and response parsing.
- **`shell_util.h`**: Executes shell commands in a separate process group, with support for live output polling and termination on cancellation.
- **`http_client.h`**: A minimalist, cancellation-aware HTTP client used for all model API calls.

### `js-bridge/` - Orchestration Layer
- **`interpreter.h`**: Implements the QuickJS environment. Provides the `run_js` tool and manages the injection of global context (`tools`, `history`, `state`).
- **`preamble.js`**: The embedded standard library for the agent's JavaScript environment. Implements high-level helpers and the `slop_guard` safety mechanism.

### Interface & Display
- **`interface/`**: Implements the terminal UI. The UI is minimal but clean, uses readline for user input, color codes and ASCII Codes.
- **`markdown/`**: Uses `tree-sitter-markdown` to provide syntax highlighting (C++, Python, Go, JS, Rust, Bash) and structured rendering for agent responses. This is a stand alone Markdown  parser / renderer library in C++.
- **`main.cpp`**: The primary event loop. Coordinates between the Orchestrator, ToolDispatcher, and UI.




### Persistent JavaScript Functions
You can persist custom JavaScript functions to the database using `tools.persist_function`. These functions are automatically loaded into the global scope of every `run_js` execution and are discoverable via `tools.help()` (JSON manifest with tool aliases and contracts).

```javascript
tools.persist_function({
  name: "calculate_tax",
  code: "return function(amount) { return amount * 0.15; }",
  description: "Calculates a 15% tax on the given amount.",
  test_args: [100],
  expected_result: 15
});
```

