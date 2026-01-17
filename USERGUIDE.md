# std::slop User Guide

## Overview
`std::slop` is a high-performance LLM CLI built for developers who want a SQL-backed, persistent conversation history with built-in tools for codebase exploration and context management.

## Installation
Build using Bazel:
```bash
bazel build //:std_slop
```

## Setup
You need an API key for either Google Gemini or OpenAI.

### For Gemini:
```bash
export GOOGLE_API_KEY="your_api_key"
```
Or use Google Cloud ADC:
```bash
gcloud auth application-default login
bazel run //:std_slop -- --google_oauth --project your-project-id
```

### For OpenAI:
```bash
export OPENAI_API_KEY="your_api_key"
```

## Running
Start a session by running the executable via Bazel. You can provide a session name to resume or categorize your work:
```bash
bazel run //:std_slop -- [session_name]
```
If no session name is provided, it defaults to `default_session`.

## Core Concepts

- **Session**: A isolated conversation history with its own settings and token usage tracking.
- **Group (GID)**: Every interaction (user prompt + assistant response + tool executions) is grouped under a unique `group_id`. This allows for atomic operations like `/undo`.
- `/commit-vibe`: Create a git commit where the message includes the last Vibe ID, the original prompt, and the current `---STATE---` blob.
- **Context**: The window of past messages sent to the LLM. It can be a rolling window of the last `N` interactions or the full history.
- **State**: The persistent "Long-term RAM" for each session.
- **Skills**: Persona patches that inject specific instructions into the system prompt.
- **Tools**: Executable functions (grep, file read, etc.) that the LLM can call.

## Slash Commands

### Session Management
- `/exit` or `/quit`: Close the session.
- `/session`: List all existing sessions.
- `/session <name>`: Switch to or create a new session named `<name>`. If the session does not exist, it will be created after the first call to the LLM.
- `/session remove <name>`: Delete a session and all its associated data (history, usage, state).

### Message Operations
- `/message list [N]`: List the prompts of the last `N` message groups.
- `/message show <GID>`: View the full content (including tool calls/responses) of a specific group.
- `/message remove <GID>`: Hard delete a specific message group from history.
- `/undo`: Delete the very last interaction group and rebuild the session context.
- `/edit`: Open your last input in your system `$EDITOR` (e.g., vim, nano) and resend it after saving.

### Context Control
- `/context`: Show current context settings and the fully assembled prompt that would be sent to the LLM.
- `/context window <N>`: Limit the context to the last `N` interaction groups. Set to `0` for infinite history.
- `/context rebuild`: Force a rebuild of the in-memory session state from the SQL message history. Useful if the database was modified externally.
- `/window <N>`: Shortcut for `/context window <N>`.

### Skills (Personas)
- `/skill list`: List all available and active skills.
- `/skill show <name|id>`: View the system prompt patch for a skill.
- `/skill activate <name|id>`: Enable a skill for the current session.
- `/skill deactivate <name|id>`: Disable a skill for the current session.
- `/skill add`: Interactive prompt to create a new skill.
- `/skill edit <name|id>`: Modify an existing skill.
- `/skill delete <name|id>`: Permanently remove a skill from the database.

#### Example Skills

it's likely that you can ask std::slop to read this file (url or local) and add these skills to your database.

**planner**
- **Description**: Strategic Tech Lead specialized in architectural decomposition and iterative feature delivery.
- **System Prompt Patch**:
```text
You only plan. You _do_ _not_ implement anything, and do not write or modify any files. You give me ideas to plan ONLY!
```

**dba**
- **Description**: Database Administrator specializing in SQLite schema design, optimization, and data integrity.
- **System Prompt Patch**:
```text
As a DBA, you are the steward of the project's data. You focus on efficient schema design, precise query construction, and maintaining data integrity. When interacting with the database: 1. Always verify schema before operations. 2. Use transactions for complex updates. 3. Provide clear explanations for schema changes. 4. Optimize for performance while ensuring clarity.
```

**c++_expert**
- **Description**: Enforces strict adherence to project C++ constraints: C++17, Google Style, no exceptions, RAII/unique_ptr, absl::Status.
- **System Prompt Patch**:
```text
You are a C++ Expert specialized in the std::slop codebase.
You MUST adhere to these constraints in every code change:
- Language: C++17.
- Style: Google C++ Style Guide.
- Exceptions: Strictly disabled (-fno-exceptions). Never use try, catch, or throw.
- Memory: Use RAII and std::unique_ptr exclusively. Avoid raw new/delete. Use stack allocation where possible.
- Error Handling: Use absl::Status and absl::StatusOr for all fallible operations.
- Threading: Avoid threading and async primitives. If necessary, use absl based primitives with std::thread and provide tsan tests.
- Design: Prefer simple, readable code over complex template metaprogramming or deep inheritance.
You ALWAYS run all the tests and ensure the affected targets compiles correctly.
```

### Tools
- `/tool list`: List all tools currently available to the agent.
- `/tool show <name>`: View the JSON Schema and description for a tool.

### Utility & Debugging
- `/stats` or `/usage`: View token usage for the current session and global totals.
- `/schema`: Display the SQLite database schema.
- `/models`: List available models for the current provider.
- `/model <name>`: Switch the active LLM model (e.g., `gemini-1.5-pro`, `gpt-4o-mini`).
- `/exec <command>`: Execute a shell command without leaving the CLI.
- `/query_db`: Direct SQL access to the session ledger.

## Database Schema
The CLI uses a local SQLite database (default: `slop.db`). You can inspect it directly:
```bash
sqlite3 slop.db
```
Tables include `messages`, `sessions`, `usage`, `skills`, `tools`, and `session_state`.
