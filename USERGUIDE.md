# std::slop User Guide

Welcome to **std::slop**, a persistent SQL-backed AI coding agent. This guide will help you understand how to use the various features of the agent to enhance your development workflow.

---

## Table of Contents
1. [Getting Started](#getting-started)
2. [Core Concepts](#core-concepts)
3. [Commands Reference](#commands-reference)
4. [Context Management](#context-management)
5. [Using Skills](#using-skills)
6. [Tool Execution](#tool-execution)
7. [Advanced Usage](#advanced-usage)

---

## Getting Started

### Installation
Ensure you have the prerequisites installed:
- Bazel 8.x
- C++17 compiler (GCC or Clang)
- `readline` development headers
- `git` (optional, for enhanced code search)

Build the project:
```bash
bazel build //...
```

### Authentication
`std::slop` supports Google Gemini (via API key or OAuth) and OpenAI-compatible APIs.

**1. Google Gemini (OAuth - Recommended):**
If no API keys are provided, the agent will prompt you to authenticate via a Google Cloud OAuth flow. This allows the agent to check your quota and manage project settings automatically.

**2. API Keys (Environment Variables):**
```bash
export GOOGLE_API_KEY="your_google_key"
# OR
export OPENAI_API_KEY="your_openai_key"
```

**3. Command-line Flags:**
```bash
bazel run //:std_slop -- --google_api_key="your_key"
bazel run //:std_slop -- --openai_api_key="your_key"
```

### Running the Agent
Start a session by running the executable via Bazel. You can provide a session name to resume or categorize your work:
```bash
bazel run //:std_slop -- [session_name]
```

---

## Core Concepts

### The Ledger (SQLite)
Everything in `std::slop` is stored in a SQLite database (`slop.db`). This ensures transparency and persistence:
- **Message History**: Every prompt, tool call, and response.
- **Tools**: Definitions of the agent's capabilities.
- **Skills**: Specialized persona instructions.
- **Usage**: Token consumption tracking.
- **State**: The persistent "Long-term RAM" for each session.

### Message Groups
Interactions are grouped by a `group_id`. A group typically encompasses a user prompt, any subsequent tool calls/results, and the assistant's final response. Commands like `/undo` and `/message remove` operate on these atomic units.

---

## Commands Reference

### General Commands
- `/help`: Show available commands and basic usage.
- `/exit` or `/quit`: Close the session.
- `/edit`: Opens `$EDITOR` for multi-line prompt input.
- `/exec <cmd>`: Runs a shell command and pipes output to a pager.
- `/throttle [N]`: Sets a delay (in seconds) between agent iterations.
- `/schema`: Displays the SQLite database schema.

### Session Management
- `/session`: List all existing sessions.
- `/session <name>`: Switch to or create a new session named `<name>`. If the session does not exist, it will be created after the first call to the LLM.
- `/stats` (or `/usage`): View message stats and Gemini user quota (if OAuth is active).

### History & Context
- `/undo`: Removes the last interaction group and rebuilds the context.
- `/context`: Show current context status and assembled prompt.
- `/context window <N>` (or `/window <N>`): Set the rolling window size (0 for full history).
- `/context rebuild`: Force a refresh of the persistent `---STATE---` block from history.
- `/context show`: Display the exact prompt being sent to the LLM.
- `/message list [N]`: List the last N interaction groups.
- `/message show <GID>` (or `view`): View a specific message group in detail.
- `/message remove <GID>`: Delete a specific message group.

---

## Context Management

`std::slop` uses a **Sequential Rolling Window** combined with a **Persistent State Anchor**.

### Rolling Window
The agent maintains a rolling window of recent interactions to keep the prompt size manageable while preserving immediate conversational flow. You can adjust this with `/window <N>`.

### Global Anchor (---STATE---)
The LLM autonomously maintains a `---STATE---` block at the end of its responses. The Orchestrator extracts this and injects it into every new prompt. This ensures that high-level goals and technical details are never lost, even when they fall out of the rolling window.

---

## Using Skills

Skills are system prompt patches that define a persona or set of constraints (e.g., "Expert C++ Developer").

- `/skill list`: List all skills and see which are active.
- `/skill show <name|id>` (or `view`): Display skill details.
- `/skill add`: Create a new skill using a template in your editor.
- `/skill edit <name|id>`: Modify an existing skill.
- `/skill delete <name|id>`: Remove a skill from the database.
- `/skill activate <name|id>`: Enable a skill for the current session.
- `/skill deactivate <name|id>`: Disable a skill for the current session.

---

## Tool Execution

The agent uses tools to interact with your system.

**Core Tools:**
- `read_file`: Read local files with line numbers.
- `write_file`: Create or update files.
- `grep_tool`: Search patterns (delegates to `git grep` if possible).
- `git_grep_tool`: Advanced git-based search.
- `execute_bash`: Run arbitrary shell commands.
- `search_code`: Simplified interface for codebase searching.
- `query_db`: Direct SQL access to the session ledger.

---

## Advanced Usage

### Inspecting the Database
Since everything is SQL-backed, you can perform advanced analysis or cleanup:
- `/exec sqlite3 slop.db "SELECT * FROM usage ORDER BY created_at DESC LIMIT 5"`
- Use `query_db` to ask the agent to analyze its own performance or token usage.

### Model Switching
- `/models`: List models supported by your current provider.
- `/model <name>`: Switch the active model on the fly.

---

## Security
**Caution:** This agent can execute shell commands and modify files. It is highly recommended to run `std::slop` in a sandboxed environment (e.g., Docker or bubblewrap) to prevent accidental damage.
