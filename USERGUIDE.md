# std::slop User Guide

Welcome to **std::slop**, a rinky dink SQL-backed AI coding agent. This guide will help you understand how to use the various features of the agent to enhance your development workflow.

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

Build the project:
```bash
bazel build //...
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
bazel run //:std_slop -- --google_api_key="your_key"
bazel run //:std_slop -- --openai_api_key="your_key"
```

### Running the Agent
Start a session by running the executable via Bazel. You can optionally provide a session ID:
```bash
bazel run //:std_slop -- my_project_session
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

Commands like `/message remove` or `/undo` operate on these atomic groups.

---

## Commands Reference

### General Commands
- `/help`: Show available commands.
- `/exit` or `/quit`: Close the session.
- `/edit`: Opens your system `$EDITOR` (e.g., vim, nano) for writing long, multi-line prompts.
- `/exec <cmd>`: Runs a shell command and pipes the output to a pager.
- `/throttle [N]`: Sets a delay (in seconds) between iterations of the agent's tool execution loop. Use `/throttle` without arguments to see the current value.

### Session Management
- `/session`: List all sessions stored in the database.
- `/session <session_id>`: Switch to a different session. Use this to start a fresh interaction or pivot to a different task while keeping currently active skills and throttles.
- `/stats`: View message counts and token usage for the current session.

### History & Context
- `/undo`: Remove the last interaction (user prompt and assistant response) and rebuild the context. Useful for correcting mistakes or retrying a prompt.
- `/context`: Show context status and the currently assembled prompt.
- `/context window <N>`: Set the rolling window size (N groups). Use 0 for full history.
- `/window <N>`: Alias for `/context window <N>`.
- `/context rebuild`: Rebuild the persistent state (---STATE--- anchor) from the current context window history.
- `/message list [N]`: Show a summary of the last N interaction groups (GIDs and user prompts).
- `/message show <GID>`: View the full content of a specific message group in your editor.
- `/message remove <GID>`: Permanently **delete** a message group from the database.

---

## Context Management

`std::slop` uses a **Sequential Rolling Window** to manage the LLM's context.

### Windowing
By default, the agent sends a rolling window of the last few interactions. You can adjust this:
- `/context window <N>`: Sets the window to the last N groups.
- `/context window 0`: Disables windowing and sends the full session history.

### Context Persistence (State)
Critical information (project goals, technical anchors) is automatically preserved in a persistent `---STATE---` block that survives windowing. This "Long-term RAM" is managed autonomously by the LLM.

---

## Using Skills

Skills are specialized instructions or "personas" you can activate.

### Managing Skills
- `/skill list`: See all available and active skills.
- `/skill show <name|id>`: Display the details of a specific skill.
- `/skill add`: Create a new skill. It will open your editor with a template:
    ```yaml
    #name: expert_coder
    #description: Focuses on C++ best practices
    #patch: You are an expert C++ developer. Always use C++17 and avoid exceptions.
    ```
- `/skill edit <name|id>`: Modify an existing skill.
- `/skill delete <name|id>`: Remove a skill from the database.

### Activating Skills
- `/skill activate <name|id>`: Add a skill to the current session. You can have multiple active skills.
- `/skill deactivate <name|id>`: Remove a skill from the current session.

---

## Tool Execution

The agent can autonomously perform tasks using tools. When it wants to use a tool, it will ask for confirmation (if configured) or just execute it.

**Available Tools:**
- `read_file`: Read contents of a local file.
- `write_file`: Create or overwrite a local file.
- `execute_bash`: Run shell commands.
- `index_directory`: Recursively index a directory for searching.
- `search_code`: Search through the indexed code using FTS5.
- `query_db`: Run SQL queries against the internal `slop.db`. This is a powerful tool. You can for example ask the llm to create and add a skill for you to do something interesting. You can use it to prune errors from the message ledger etc.

---

## Advanced Usage

### Searching Your Codebase
To allow the agent to "see" your project:
1. Use `/exec` or the agent's `execute_bash` to explore the directory. /exec is useful for simple stuff like /exec git diff.
2. Ask the agent to index the directory: "Index the current directory for code search." It's indexed in sqlite of course.
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


### Tool Management
- `/tool list`: List all enabled tools.
- `/tool show <name>`: Display the JSON schema and description of a specific tool.
