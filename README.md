# std::slop

std::slop is a sqlite based c++ cli agent. It uses a small per project database to remember everything you do, so it never loses track of your work. Most of the agentic work that it does is db driven, for persistence and longer term recall. It has first class support for git. There is tooling to teach the LLM std::slop's schema, so you can just import and export data, such as skills, when needed.

## Distinguishing Features

- **Ledger-Driven**: All interactions, tool calls, and system changes are stored in SQLite.
- **Multi Model support**: Supports Google Gemini (via API key or OAuth) and OpenAI-compatible APIs (defaults to OpenRouter). For OpenRouter models, `--strip_reasoning` is available for better compatibility with reasoning-enabled models.
- **Strategy-Aware Replay**: Automatically re-parses historical conversation text when switching models mid-session. Tool calls are isolated by provider to keep things simple while avoiding cross-model parsing errors.
- **Context Control**: Read existing context, manipulate the ledger to remove things from the context, or have complete isolation for multiple context streams through sessions.
- **Sequential Rolling Window**: Narrative coherence through chronological history windowing.
- **Historical Context Retrieval**: Ability for the agent to query its own past history via SQL, allowing it to regain context that has fallen out of the rolling window.
- **Self-Managed State**: Persistent "Long-term RAM" block (---STATE---) autonomously updated by the LLM as part of system prompt. 
- **Session Scratchpad**: A task oriented, persistent markdown workspace for evolving plans, checklists, and task-specific notes. The LLM can introspect and update this autonomously. Mandatory for structured planning and tracking. Largely 'self managed' by the LLM.
- **Semantic Memo System**: Long-term knowledge persistence through tag-based memos. Memos are automatically retrieved based on conversation context to guide the LLM, ensuring architectural and technical decisions persist across sessions. Build up project specific knowledge over time. This is largely 'self managed' by the LLM.
- **Dynamic Skill Orchestration**: The agent proactively searches for and "self-activates" specialized skills (Planner, Code Reviewer, DBA) based on your request, automatically returning to the core persona when the task is complete.
- **Live Code Search**: Heavy reliance on  `git grep` (with standard `grep` fallback), for rich context and line numbers without indexing overhead. `git grep` supports function context based searching, so that reads are context efficient.
- **Transparent Context**: Real-time display of estimated context token counts and structural delimiters (`--- BEGIN HISTORY ---`, etc.) to see exactly what the LLM sees. The token counters are tracked in the SQL DB to present fine grained usage stats per task.
- **Enhanced UI**: Tree Sitter based Markdown support. Uses ANSI-colored output for improved readability, featuring distinct headers for assistant responses and tool executions. UI is also deliberately kept simple without frills.
- **Intelligent Table Wrapping**: Markdown tables are automatically shrunk and word-wrapped to fit your terminal width, ensuring no data is lost even in narrow windows.
- **Output Truncation**: Smart truncation of tool calls and results to 60 columns to maintain terminal clarity while preserving relevant context.
- **Readline Support**: Command history and line auto-completion.
- **Skills System**: Inject specialized personas and instructions into the session.

## Architecture

- **Storage**: SQLite3 (Ledger, Tools, Skills, State, Scratchpad).
- **Orchestrator**: Unified logic for prompt assembly and response processing.
- **Execution**: Secure tool execution engine.
- **Network**: Asynchronous HTTP client with automatic exponential backoff for 429/5xx errors.

## Codebase Layout

- **`core/`**: The engine of `std::slop`. Contains database management, the Orchestrator (logic for model interaction), HTTP clients, and tool execution logic.
- **`interface/`**: User interface components, including terminal UI, command handling, and line completion.
- **`markdown/`**: A self-contained library for parsing and rendering Markdown to terminal-ready ANSI-styled strings using tree-sitter. Might be split out later. 
- **`scripts/`**: Utility scripts for linting, formatting.
- **`main.cpp`**: Application entry point and global initialization.

## Installation

### Prerequisites

- C++17 compiler (Clang or GCC).
- Bazel 8.x (Bazelisk recommended).
- `readline` development headers.
- **Git**: Git is central to the workflow of `std::slop`. The tool has a hard reliance on its features for code exploration, state tracking, and change management. **Repositories that use `std::slop` must be valid git repositories.**

### Build

```bash
bazel build //...
```

## Development

The project includes hermetic tools for code formatting and linting, managed via Bazel to ensure consistency across environments.

### Code Formatting

We use `clang-format` (LLVM 17.0.6) with the Google style guide.

**Check formatting:**
```bash
bazel run //:format.check
# OR
bazel test //:format_test
```

**Apply formatting:**
```bash
bazel run //:format
```

### SQL Security

To prevent SQL injection, **never** use string concatenation or `absl::Substitute` with user-supplied values when interacting with the database. Always use the parameterized versions of `Database::Query` and `Database::Execute`:

```cpp
// Correct
db_->Query("SELECT * FROM messages WHERE group_id = ?", {sub_args});

// Incorrect
db_->Query("SELECT * FROM messages WHERE group_id = '" + sub_args + "'");
```

### Linting

We use `clang-tidy` for static analysis. Configuration is in `.clang-tidy`.
Code is `asan` and `tsan` clean.

**Run linter:**
```bash
bazel test //:clang_tidy_test
```

### Unified Check

A helper script is provided to run all checks:

```bash
./scripts/lint.sh
```

### Logging

The project uses [Abseil Logging](https://abseil.io/docs/cpp/guides/logging).

**Basic Usage:**
- `LOG(INFO)`, `LOG(WARNING)`, `LOG(ERROR)` are used for general events and errors.
- `VLOG(1)` is used for detailed tracking (e.g., request headers).
- `VLOG(2)` is used for full data dumps (e.g., request/response bodies).

**Viewing Logs:**
By default, logs are written to stderr. You can control the verbosity and destination using standard Abseil flags:

```bash
# Show all INFO logs and above to stderr
bazel run //:std_slop -- --stderrthreshold=0

# Enable verbose logging for request/response bodies
bazel run //:std_slop -- --v=2 --stderrthreshold=0

# Log to a specific file
bazel run //:std_slop -- --log=slop.log
```

## Usage

For a step-by-step guide, see the [WALKTHROUGH](WALKTHROUGH.md).

### 1. Setup API Keys

```bash
export GOOGLE_API_KEY="key"
export OPENAI_API_KEY="key"
```

Or use command-line flags: `--google_api_key`, `--openai_api_key`. If no keys are provided, the agent will attempt an OAuth login flow for Google Gemini.

### 3. Run

```bash
bazel run //:std_slop -- [session_name]
```

## Security

**Caution:** This agent can execute shell commands and modify your file system. It is highly recommended to run this project in a sandboxed environment such as **bubblewrap** or **Docker** to prevent accidental or malicious damage to your system.

## Code

- C++ Standard: C++17.
- Style: Google C++ Style Guide.
- Exceptions: Disabled (-fno-exceptions).
- Memory: RAII and std::unique_ptr exclusively.
- Error Handling: absl::Status and absl::StatusOr.
- Avoid threading and async primitives, if they must be used, use absl based primitives with std::thread. Any threading workflow requires tsan tests.
- Asan and Tsan clean at all time.

## Other Documentation

* [WALKTHROUGH](WALKTHROUGH.md)
* [CONTRIBUTING](CONTRIBUTING.md)
* [SESSIONS](SESSIONS.md)
* [OAUTH](OAUTH.md)
* [CONTEXT_MANAGEMENT](CONTEXT_MANAGEMENT.md)
* [USERGUIDE](USERGUIDE.md)
* [SCHEMA](SCHEMA.md)
* [system_prompt](system_prompt.md)

