# std::slop

std::slop is a C++17 AI coding agent driven by a persistent SQLite ledger for session management and transparency.

## Features

- Ledger-Driven: All interactions, tool calls, and system changes are stored in SQLite.
- Dual API: Supports Google Gemini and OpenAI-compatible APIs.
- Context Control: Manage memory via group-based drop and rebuild commands.
- Hybrid Retrieval: Weighted Reciprocal Rank Fusion (RRF) combining FTS5 keyword relevance (1.5x) and chronological recency (1.0x).
- Tool Execution: Autonomous local file system and shell operations.
- Code Search: FTS5 virtual tables for full-text search.
- Readline Support: Command history and line auto-completion.

## Architecture

- Storage: SQLite3 (Ledger, Tools, Skills, FTS5).
- Orchestrator: Unified logic for prompt assembly, RRF ranking, and response processing.
- Execution: Secure tool execution engine.
- Network: Asynchronous communication via libcurl.

## Documentation

- SCHEMA.md: Database schema details.
- SESSIONS.md: Session isolation and mechanics.
- PLAN.md: Roadmap and phases.

## Building and Running

### Prerequisites

- CMake 3.10+
- C++17 compiler
- libcurl and readline headers

### 1. Build

```bash
mkdir build && cd build
cmake ..
make std::slop
```

### 2. Authentication

Set environment variables:

```bash
export GOOGLE_API_KEY="key"
export OPENAI_API_KEY="key"
```

### 3. Run

```bash
./std::slop [session_id]
```

## Security

**Caution:** This agent can execute shell commands and modify your file system. It is highly recommended to run this project in a sandboxed environment such as **bubblewrap** or **Docker** to prevent accidental or malicious damage to your system.

## Command Reference

### Context and History

- /context show [N]: Show last N messages of active history.
- /message list [N]: List last N entries keyed by Group ID.
- /message view [G]: View message group G in editor.
- /message remove [G]: Delete message group G from database.
- /message drop [G]: Hide message group G from context.
- /undo: Revert the last interaction group.

### Context Management

- /context drop: Hide all messages from current context for this session.
- /context build [N]: Reactivate last N interaction groups.
- /context-mode fts <N>: Enable hybrid FTS-Ranked retrieval (N groups).
- /context-mode full: Disable ranking and send all available history.

### Knowledge & Skills

- /skills: List available personas (chat, planner, expert_coder).
- /skill activate [Name|ID]: Switch the active agent persona.
- /sessions: List all conversation sessions.
- /switch [ID]: Switch to a different session.
- /schema: Print internal SQL schema.
- /stats: Show session message statistics.

## Project Constraints

- C++ Standard: C++17.
- Style: Google C++ Style Guide.
- Exceptions: Disabled (-fno-exceptions).
- Memory: RAII and std::unique_ptr exclusively.
- Error Handling: absl::Status and absl::StatusOr exclusively.
