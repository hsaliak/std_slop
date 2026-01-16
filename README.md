# std::slop

std::slop is a C++17 AI coding agent driven by a persistent SQLite ledger for session management and transparency.

## Features

- **Ledger-Driven**: All interactions, tool calls, and system changes are stored in SQLite.
- **Dual API**: Supports Google Gemini (via API key or OAuth) and OpenAI-compatible APIs.
- **Context Control**: Manage memory via group-based rebuild commands.
- **Sequential Rolling Window**: Maintains narrative coherence through chronological history windowing.
- **Self-Managed State**: Persistent "Long-term RAM" block (---STATE---) autonomously updated by the LLM.
- **Live Code Search**: Instant codebase exploration using `git grep` (with standard `grep` fallback), providing rich context and line numbers without indexing overhead.
- **Transparent Context**: Real-time display of estimated context token counts and structural delimiters (`--- BEGIN HISTORY ---`, etc.) to see exactly what the LLM sees.
- **Tool Execution**: Autonomous local file system and shell operations.
- **Readline Support**: Command history and line auto-completion.
- **Skills System**: Inject specialized personas and instructions into the session.

## Architecture

- **Storage**: SQLite3 (Ledger, Tools, Skills, State).
- **Orchestrator**: Unified logic for prompt assembly and response processing.
- **Execution**: Secure tool execution engine.
- **Network**: Asynchronous HTTP client with automatic exponential backoff for 429/5xx errors.

## Installation

### Prerequisites

- C++17 compiler (Clang or GCC).
- Bazel 8.x (Bazelisk recommended).
- `readline` development headers.
- `git` (optional, for enhanced code search).

### Build

```bash
bazel build //...
```

## Usage

### 1. Setup API Keys

```bash
export GOOGLE_API_KEY="key"
export OPENAI_API_KEY="key"
```

Or use command-line flags: `--google_api_key`, `--openai_api_key`.

### 3. Run

```bash
bazel run //:std_slop -- [session_id]
```

## Security

**Caution:** This agent can execute shell commands and modify your file system. It is highly recommended to run this project in a sandboxed environment such as **bubblewrap** or **Docker** to prevent accidental or malicious damage to your system.

## Command Reference

### Session and Context
- `/session [ID]`    List all sessions or switch to a specific one.
- `/context`         Show context status and assembled prompt.
- `/context window <N>` Set size of rolling window (0 for full history).
- `/window <N>`      Alias for `/context window <N>`.
- `/context rebuild` Rebuild session state from conversation history.

### Skills and Tools
- `/skill list`      List available skills.
- `/skill show <ID>` Display details of a skill.
- `/tool list`       List enabled tools.
- `/tool show <name>` Show tool schema and description.

### Models and Settings
- `/models`          List available models from the provider.
- `/model <name>`    Switch the active LLM model.
- `/throttle [N]`    Set or show request throttle (seconds) for agentic loops.
- `/exec <command>`  Run a shell command and view output in a pager.
- `/edit`            Open `$EDITOR` for multi-line input.
- `/stats /usage`    Show session usage statistics.

## Built-in Tools

| Tool | Description |
| :--- | :--- |
| `read_file` | Read local file contents with automatic line numbering. |
| `write_file` | Create or overwrite local files. |
| `grep_tool` | Search for patterns with context (delegates to `git grep` when possible). |
| `search_code` | Live codebase search using optimized grep logic. |
| `execute_bash` | Run arbitrary shell commands. |
| `query_db` | Query the session ledger using SQL. |

## Project Constraints

- C++ Standard: C++17.
- Style: Google C++ Style Guide.
- Exceptions: Disabled (-fno-exceptions).
- Memory: RAII and std::unique_ptr exclusively.
- Error Handling: absl::Status and absl::StatusOr.
- Avoid threading and async primitives, if they must be used, use absl based primitives with std::thread. Any threading workflow requires tsan tests.
