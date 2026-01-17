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
| tool_call_id | TEXT | Metadata for linking responses (e.g., `id\|name`). |
| status | TEXT | `completed`, `tool_call`, or `dropped`. Default: `completed`. |
| created_at | DATETIME | Entry timestamp. Default: `CURRENT_TIMESTAMP`. |
| group_id | TEXT | Turn identifier for atomic operations (Unix nanoseconds). |

### 2. tools
Registry of available agent tools.

| Column | Type | Description |
| :--- | :--- | :--- |
| name | TEXT | Primary Key. Unique tool name. |
| description | TEXT | Functional description. |
| json_schema | TEXT | Argument definition in JSON Schema format. |
| is_enabled | INTEGER | Status flag (1 for active, 0 for inactive). Default: 1. |

### 3. skills
Persona patches for specialized instructions.

| Column | Type | Description |
| :--- | :--- | :--- |
| id | INTEGER | Primary Key (Autoincrement). |
| name | TEXT | Unique skill name. |
| description | TEXT | Purpose description. |
| system_prompt_patch| TEXT | Instructions to inject into the system prompt. |

### 4. sessions
Persists user settings for each conversation session.

| Column | Type | Description |
| :--- | :--- | :--- |
| id | TEXT | Primary Key. Session ID. |
| context_size | INTEGER | Size of the sequential rolling window (number of groups). Default: 5. |

### 5. usage
Tracks token usage for cost and performance monitoring.

| Column | Type | Description |
| :--- | :--- | :--- |
| id | INTEGER | Primary Key (Autoincrement). |
| session_id | TEXT | Associated session ID. |
| model | TEXT | Model name used for the interaction. |
| prompt_tokens | INTEGER | Tokens in the prompt. |
| completion_tokens | INTEGER | Tokens in the response. |
| total_tokens | INTEGER | Sum of prompt and completion tokens. |
| created_at | DATETIME | Timestamp of the interaction. Default: `CURRENT_TIMESTAMP`. |

### 6. session_state
Stores the persistent self-managed state block.

| Column | Type | Description |
| :--- | :--- | :--- |
| session_id | TEXT | Primary Key. Session ID. |
| state_blob | TEXT | The persistent `---STATE---` block. |
| last_updated | TIMESTAMP | Timestamp of last update. Default: `CURRENT_TIMESTAMP`. |

## Default Tools

The following tools are registered by default during database initialization:

- `grep_tool`: Search for a pattern in the codebase.
- `git_grep_tool`: Optimized search for git repositories.
- `read_file`: Read file contents.
- `write_file`: Write file contents.
- `execute_bash`: Run shell commands.
- `search_code`: Codebase search via grep.
- `query_db`: Direct SQLite querying.

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
    group_id TEXT
);

CREATE TABLE IF NOT EXISTS tools (
    name TEXT PRIMARY KEY,
    description TEXT,
    json_schema TEXT,
    is_enabled INTEGER DEFAULT 1
);

CREATE TABLE IF NOT EXISTS skills (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT UNIQUE,
    description TEXT,
    system_prompt_patch TEXT
);

CREATE TABLE IF NOT EXISTS sessions (
    id TEXT PRIMARY KEY,
    context_size INTEGER DEFAULT 5
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
```
