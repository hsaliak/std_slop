# std::slop

std::slop is a C++17 AI coding agent driven by a persistent SQLite ledger for session management and transparency.

## Features

- **Ledger-Driven**: All interactions, tool calls, and system changes are stored in SQLite.
- **Dual API**: Supports Google Gemini (via API key or OAuth) and OpenAI-compatible APIs.
- **Context Control**: Manage memory via group-based drop and rebuild commands.
- **Hybrid Retrieval**: Weighted Reciprocal Rank Fusion (RRF) combining FTS5 keyword relevance (1.5x) and chronological recency (1.0x).
- **Tool Execution**: Autonomous local file system and shell operations.
- **Code Search**: FTS5 virtual tables for full-text search.
- **Readline Support**: Command history and line auto-completion.
- **Skills System**: Inject specialized personas and instructions into the session.

## Architecture

- **Storage**: SQLite3 (Ledger, Tools, Skills, FTS5).
- **Orchestrator**: Unified logic for prompt assembly, RRF ranking, and response processing.
- **Execution**: Secure tool execution engine.
- **Network**: Asynchronous communication via libcurl.

## Documentation

- [SCHEMA.md](SCHEMA.md): Database schema details.
- [SESSIONS.md](SESSIONS.md): Session isolation and mechanics.
- [USERGUIDE.md](USERGUIDE.md): Comprehensive user guide and examples.

## Building and Running

### Prerequisites

- CMake 3.10+
- C++17 compiler
- libcurl and readline headers
- `nlohmann-json`, `absl` (usually fetched via CMake)

### 1. Build

```bash
mkdir build && cd build
cmake ..
make std_slop
```

### 2. Authentication

Set environment variables:

```bash
export GOOGLE_API_KEY="key"
export OPENAI_API_KEY="key"
```

Or use command-line flags: `--google_api_key`, `--openai_api_key`.

### 3. Run

```bash
./std_slop [session_id]
```

## Security

**Caution:** This agent can execute shell commands and modify your file system. It is highly recommended to run this project in a sandboxed environment such as **bubblewrap** or **Docker** to prevent accidental or malicious damage to your system.

## Command Reference

### Context and History

- `/context show`: Show active history in current context.
- `/context drop`: Hide all messages from current context for this session.
- `/context build [N]`: Reactivate last N interaction groups.
- `/message list [N]`: List last N entries with Group IDs.
- `/message view <GID>`: View message group GID in editor.
- `/message remove <GID>`: Delete message group GID from database.
- `/message drop <GID>`: Hide message group GID from context.
- `/undo`: Revert the last interaction group.

### Context Management

- `/context-mode fts <N>`: Enable hybrid FTS-Ranked retrieval (top N groups).
- `/context-mode full`: Disable ranking and send all available history.

### Knowledge & Skills

- `/skill list`: List available personas.
- `/skill activate <Name|ID>`: Enable a skill for the current session.
- `/skill deactivate <Name|ID>`: Disable an active skill.
- `/skill add`: Create a new skill using your `$EDITOR`.
- `/skill edit <Name|ID>`: Edit an existing skill.
- `/skill view <Name|ID>`: Show skill details.
- `/skill delete <Name|ID>`: Remove a skill.

### Session & System

- `/sessions`: List all conversation sessions.
- `/switch <ID>`: Switch to a different session.
- `/schema`: Print internal SQL schema.
- `/stats`: Show session message and token usage statistics.
- `/models`: List available models from the provider.
- `/model <name>`: Switch the active LLM model.
- `/throttle [N]`: Set or show request throttle (seconds) for agentic loops.
- `/exec <command>`: Run a shell command and view output in a pager.
- `/edit`: Open `$EDITOR` for multi-line input.

## Project Constraints

- C++ Standard: C++17.
- Style: Google C++ Style Guide.
- Exceptions: Disabled (-fno-exceptions).
- Memory: RAII and std::unique_ptr exclusively.
- Error Handling: absl::Status and absl::StatusOr exclusively.
- Threading: Threading and `std::future` are to be avoided. If threading is absolutely necessary, only Abseil threading primitives are allowed.
