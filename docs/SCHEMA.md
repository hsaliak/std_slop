# std::slop Database Schema
This document describes the SQLite schema used by std::slop to persist history, tools, skills, and usage statistics.
## Tables
### 1. messages
Stores user prompts, assistant responses, and tool executions.
| Column | Type | Description |
| :--- | :--- | :--- |
| id | INTEGER | Primary Key (Autoincrement). |
| session_id | TEXT | Conversation identifier. |
| role | TEXT | `system`, `user`, `assistant`, or `tool`. Has a CHECK constraint. |
| content | TEXT | Message text or tool JSON. |
| tool_call_id | TEXT | Metadata for linking responses (e.g., `id|name`). |
| status | TEXT | `completed`, `tool_call`, or `dropped`. Default: `completed`. |
| created_at | DATETIME | Entry timestamp. Default: `CURRENT_TIMESTAMP`. |
| group_id | TEXT | Turn identifier for atomic operations (Unix nanoseconds). |
| parsing_strategy | TEXT | Metadata on how the response was parsed. |
### 2. tools
Registers available tools and their schemas.
| Column | Type | Description |
| :--- | :--- | :--- |
| name | TEXT | Primary Key. |
| description | TEXT | Tool documentation for the LLM. |
| json_schema | TEXT | Arguments schema. |
| is_enabled | INTEGER | 1 if the tool can be called directly, 0 otherwise. |
| call_count | INTEGER | Usage statistic. |
> **Note**: By default, only `query_db`, `run_lua`, and `llm_query` are enabled (`is_enabled=1`). All other tools must be accessed via the `run_lua` orchestration layer.
### 3. skills
Persistent system prompt fragments and personas.
| Column | Type | Description |
| :--- | :--- | :--- |
| id | INTEGER | Primary Key (Autoincrement). |
| name | TEXT | Unique identifier for the skill. |
| description | TEXT | High-level summary of the skill's purpose. |
| system_prompt_patch | TEXT | The Markdown fragment to inject into the system prompt. |
| activation_count | INTEGER | Usage statistic. |
### 4. sessions
Stores conversation-specific configuration and ephemeral state.
| Column | Type | Description |
| :--- | :--- | :--- |
| id | TEXT | Primary Key (Session UUID). |
| context_size | INTEGER | Number of messages to include in context. |
| scratchpad | TEXT | Persistent Markdown notes for the agent. |
| active_skills | TEXT | JSON array of skill names currently active. |
### 5. usage
Token usage statistics per model and session.
| Column | Type | Description |
| :--- | :--- | :--- |
| id | INTEGER | Primary Key (Autoincrement). |
| session_id | TEXT | Associated session. |
| model | TEXT | Model name (e.g., `gpt-4o`). |
| prompt_tokens | INTEGER | |
| completion_tokens | INTEGER | |
| total_tokens | INTEGER | |
| created_at | DATETIME | Entry timestamp. |
### 6. session_state
Generic key-value store for session-specific binary or JSON state.
| Column | Type | Description |
| :--- | :--- | :--- |
| session_id | TEXT | Primary Key. |
| state_blob | TEXT | Encoded state data. |
| last_updated | TIMESTAMP | |
### 7. llm_memos
Long-term knowledge persistence through tag-based memos.
| Column | Type | Description |
| :--- | :--- | :--- |
| id | INTEGER | Primary Key (Autoincrement). |
| content | TEXT | Memo text content. |
| semantic_tags | TEXT | JSON-formatted array of tags for search and retrieval. |
| created_at | DATETIME | Entry timestamp. Default: `CURRENT_TIMESTAMP`. |
### 8. patch_approvals
Tracks user approvals for specific commit hashes on branches. This is part of the Mail Model workflow.
| Column | Type | Description |
| :--- | :--- | :--- |
| branch_name | TEXT | The name of the staging branch (Primary Key). |
| approved_hash | TEXT | The git commit hash that was approved. |
| approved_at | DATETIME | Timestamp of approval. |
### 9. settings
Global application settings persisted across sessions.
| Column | Type | Description |
| :--- | :--- | :--- |
| id | INTEGER | Primary Key (fixed at 1). |
| mode | TEXT | Current operational mode (`standard` or `mail`). |
## Tool Manifest
The following tools are registered by default:
### Enabled by Default
- `query_db`: Query the local SQLite database using SQL.
- `run_lua`: Execute an orchestrated Lua 5.4 script with access to all tools and async capabilities.
- `llm_query`: Perform an isolated sub-task query to the LLM (synchronous).
### Disabled by Default (Access via `run_lua`)
- `read_file`: Read the content of a file from the local filesystem.
- `write_file`: Write content to a file in the local filesystem.
- `apply_patch`: Applies partial changes to a file by matching a specific block of text and replacing it.
- `execute_bash`: Execute a bash command on the local system.
- `list_directory`: List files and directories with optional depth and git awareness.
- `describe_db`: Describe the database schema and tables.
- `manage_scratchpad`: Read or update the persistent session-specific scratchpad.
- `save_memo`: Save a memo with semantic tags for later retrieval.
- `retrieve_memos`: Retrieve memos based on semantic tags.
- `use_skill`: Activate or deactivate a specialized skill/persona.
- `grep_tool` / `git_grep_tool`: Codebase searching tools.
> **Note**: `search_code` is a high-level wrapper defined in `preamble_lib.lua` that uses `grep_tool`.
## Default Skills
The following skills are registered by default:
- `planner`: Tech Lead specialized in architectural decomposition and iterative feature delivery.
- `dba`: Database Administrator specializing in SQLite schema design and data integrity.
- `c++_expert`: Enforces strict adherence to project C++17 constraints and Google style.
- `code_reviewer`: Multilingual code reviewer enforcing language-specific standards (Google C++, PEP8, etc.).
- `lua_control_plane`: Restricts the agent to using only `run_lua` for all operations, ensuring reproducibility.
- `patcher`: Specialist in the Mail Model workflow for atomic, bisect-safe commits.
## SQL Initialization
```sql
CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT,
    role TEXT CHECK(role IN ('system', 'user', 'assistant', 'tool')),
    content TEXT,
    tool_call_id TEXT,
    status TEXT DEFAULT 'completed',
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    group_id TEXT,
    parsing_strategy TEXT
);
CREATE TABLE IF NOT EXISTS tools (
    name TEXT PRIMARY KEY,
    description TEXT,
    json_schema TEXT,
    is_enabled INTEGER DEFAULT 1,
    call_count INTEGER DEFAULT 0
);
CREATE TABLE IF NOT EXISTS skills (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT UNIQUE,
    description TEXT,
    system_prompt_patch TEXT,
    activation_count INTEGER DEFAULT 0
);
CREATE TABLE IF NOT EXISTS sessions (
    id TEXT PRIMARY KEY,
    context_size INTEGER DEFAULT 3,
    scratchpad TEXT,
    active_skills TEXT
);
CREATE TABLE IF NOT EXISTS usage (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT,
    model TEXT,
    prompt_tokens INTEGER,
    completion_tokens INTEGER,
    total_tokens INTEGER,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS session_state (
    session_id TEXT PRIMARY KEY,
    state_blob TEXT,
    last_updated TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS llm_memos (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    content TEXT NOT NULL,
    semantic_tags TEXT NOT NULL,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS patch_approvals (
    branch_name TEXT PRIMARY KEY,
    approved_hash TEXT NOT NULL,
    approved_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS settings (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    mode TEXT NOT NULL DEFAULT 'standard'
);
```
