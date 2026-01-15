# std::slop User Guide

Welcome to **std::slop**, a powerful, SQL-backed AI coding agent. This guide will help you understand how to use the various features of the agent to enhance your development workflow.

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
- CMake 3.10+
- C++17 compiler (GCC or Clang)
- `libcurl` and `readline` development headers

Build the project:
```bash
mkdir build && cd build
cmake ..
make std_slop
```

### Authentication
You can authenticate with either Google Gemini or OpenAI:

**Environment Variables:**
```bash
export GOOGLE_API_KEY="your_google_key"
# OR
export OPENAI_API_KEY="your_openai_key"
```

**Command-line Flags:**
```bash
./std_slop --google_api_key="your_key"
./std_slop --openai_api_key="your_key"
```

### Running the Agent
Start a session by running the executable. You can optionally provide a session ID:
```bash
./std_slop my_project_session
```

---

## Core Concepts

### The Ledger (SQLite)
Everything in `std::slop` is backed by a SQLite database (`slop.db` by default). This includes:
- **Message History**: Every prompt and response.
- **Tools**: Definitions of what the agent can do.
- **Skills**: System prompt patches for specific personas.
- **Usage**: Token consumption tracking.

### Message Groups
Interactions are grouped by a `group_id`. A typical group contains:
1. User prompt
2. Assistant's tool calls (if any)
3. Tool execution results
4. Assistant's final response

Commands like `/undo` operate on these atomic groups.

---

## Commands Reference

### General Commands
- `/help`: Show available commands.
- `/exit` or `/quit`: Close the session.
- `/edit`: Opens your system `$EDITOR` (e.g., vim, nano) for writing long, multi-line prompts.
- `/exec <cmd>`: Runs a shell command and pipes the output to a pager.
- `/throttle [N]`: Sets a delay (in seconds) between iterations of the agent's tool execution loop. Use `/throttle` without arguments to see the current value.

### Session Management
- `/sessions`: List all sessions stored in the database.
- `/switch <session_id>`: Switch to a different session history.
- `/stats`: View message counts and token usage for the current session.

### History & Context
- `/context show`: Display the messages currently being sent to the LLM.
- `/context drop`: Hide all previous messages from the LLM (they remain in the DB).
- `/context build [N]`: Re-enable the last N message groups.
- `/message list [N]`: Show the last N message groups with their IDs.
- `/message view <GID>`: View the full content of a specific message group in your editor.
- `/undo`: Quickly "forget" the last interaction group.

---

## Context Management

`std::slop` supports two context modes:

### Full Context (Default)
Sends the entire conversation history of the current session to the LLM. 
- **Enable**: `/context-mode full`

### FTS-Ranked Context
Uses a hybrid search (BM25 + Recency) to find the most relevant message groups for your current query. This is useful for very long conversations that exceed the LLM's context window.
- **Enable**: `/context-mode fts <N>` (where N is the number of groups to retrieve)

---

## Using Skills

Skills are specialized instructions or "personas" you can activate.

### Managing Skills
- `/skill list`: See all available and active skills.
- `/skill add`: Create a new skill. It will open your editor with a template:
    ```yaml
    #name: expert_coder
    #description: Focuses on C++ best practices
    #patch: You are an expert C++ developer. Always use C++17 and avoid exceptions.
    ```
- `/skill edit <name>`: Modify an existing skill.
- `/skill delete <name>`: Remove a skill from the database.

### Activating Skills
- `/skill activate <name>`: Add a skill to the current session. You can have multiple active skills.
- `/skill deactivate <name>`: Remove a skill from the current session.

---

## Tool Execution

The agent can autonomously perform tasks using tools. When it wants to use a tool, it will ask for confirmation (if configured) or just execute it.

**Available Tools:**
- `read_file`: Read contents of a local file.
- `write_file`: Create or overwrite a local file.
- `execute_bash`: Run shell commands.
- `index_directory`: Recursively index a directory for searching.
- `search_code`: Search through the indexed code using FTS5.
- `query_db`: Run SQL queries against the internal `slop.db`.

---

## Advanced Usage

### Searching Your Codebase
To allow the agent to "see" your project:
1. Use `/exec` or the agent's `execute_bash` to explore the directory.
2. Ask the agent to index the directory: "Index the current directory for code search."
3. Once indexed, the agent can use `search_code` to find relevant snippets across your entire project.

### Inspecting the Database
You can directly view the internal state of the agent:
- `/schema`: See the tables and columns.
- `/exec sqlite3 slop.db "SELECT * FROM usage"`: Manual SQL inspection.

### Customizing the Model
You can switch models on the fly:
- `/models`: List available models from your provider.
- `/model <model_name>`: Switch (e.g., `/model gpt-4-turbo` or `/model gemini-1.5-pro`).

---

## Security Tip
Always run `std::slop` in a directory you are comfortable with it modifying, or better yet, in a containerized environment.
