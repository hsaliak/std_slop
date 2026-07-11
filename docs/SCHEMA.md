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
| tokens | INTEGER | Token count for the message. Default: 0. |

### 2. tools

Registers available tools and their JSON schemas.

| Column | Type | Description |
| :--- | :--- | :--- |
| name | TEXT | Primary Key. Tool identifier. |
| description | TEXT | Human-readable tool description. |
| json_schema | TEXT | JSON schema for tool parameters. |
| is_enabled | INTEGER | Whether the tool is active. Default: 1. |
| call_count | INTEGER | Number of times the tool has been invoked. Default: 0. |

### 3. skills

Stores LLM-generated skill definitions that modify system behavior.

| Column | Type | Description |
| :--- | :--- | :--- |
| id | INTEGER | Primary Key (Autoincrement). |
| name | TEXT | Unique skill name. |
| description | TEXT | Skill purpose. |
| system_prompt_patch | TEXT | Partial system prompt to inject. |
| activation_count | INTEGER | Number of times the skill has been activated. Default: 0. |

### 4. sessions

Tracks active conversation sessions.

| Column | Type | Description |
| :--- | :--- | :--- |
| id | TEXT | Primary Key. Session identifier. |
| accordion_retain_groups | INTEGER | Complete interaction groups retained after accordion reset. Default: 2. |
| accordion_watermark_tokens | INTEGER | Latest actual prompt-token threshold that resets context before the next generation. Default: 350000. |
| accordion_epoch_start_group_id | TEXT | First group in the append-only accordion epoch; reset to the oldest retained group. |
| active_skills | TEXT | Comma-separated list of active skill names. |
| parent_session_id | TEXT | Parent session for branching conversations. |
| depth | INTEGER | Nesting depth of the session. Default: 0. |
| is_ephemeral | INTEGER | Whether the session is ephemeral. Default: 0. |

### 5. usage

Logs LLM API usage statistics.

| Column | Type | Description |
| :--- | :--- | :--- |
| id | INTEGER | Primary Key (Autoincrement). |
| session_id | TEXT | Associated session. |
| model | TEXT | Model identifier (e.g., `claude-3-opus`). |
| prompt_tokens | INTEGER | Tokens in the prompt. |
| completion_tokens | INTEGER | Tokens in the completion. |
| total_tokens | INTEGER | Sum of prompt and completion tokens. |
| created_at | DATETIME | Timestamp. Default: `CURRENT_TIMESTAMP`. |

### 6. session_state

Persists arbitrary session state as a blob.

| Column | Type | Description |
| :--- | :--- | :--- |
| session_id | TEXT | Primary Key. |
| state_blob | TEXT | Serialized state data. |
| last_updated | TIMESTAMP | Last modification time. Default: `CURRENT_TIMESTAMP`. |

### 7. llm_memos

Stores semantic memories created by the LLM.

| Column | Type | Description |
| :--- | :--- | :--- |
| id | INTEGER | Primary Key (Autoincrement). |
| content | TEXT | Memo content. |
| semantic_tags | TEXT | Comma-separated tags for categorization. |
| created_at | DATETIME | Timestamp. Default: `CURRENT_TIMESTAMP`. |

### 7.5 scratchpads

Stores a session-specific scratchpad buffer.

| Column | Type | Description |
| :--- | :--- | :--- |
| session_id | TEXT | Primary Key. References `sessions.id`. |
| content | TEXT | Scratchpad content. |
| updated_at | DATETIME | Last write time. Default: `CURRENT_TIMESTAMP`. |

### 8. patch_approvals

Tracks approved git patches for automated application.

| Column | Type | Description |
| :--- | :--- | :--- |
| branch_name | TEXT | Primary Key. Branch name. |
| approved_hash | TEXT | Git hash that has been approved. |
| approved_at | DATETIME | Approval timestamp. Default: `CURRENT_TIMESTAMP`. |

### 9. staging_branches

Manages git branches for staged changes.

| Column | Type | Description |
| :--- | :--- | :--- |
| branch_name | TEXT | Primary Key. Branch name. |
| parent_branch | TEXT | Base branch from which this branch was created. |
| created_at | DATETIME | Creation timestamp. Default: `CURRENT_TIMESTAMP`. |

### 10. settings

Global application settings.

| Column | Type | Description |
| :--- | :--- | :--- |
| id | INTEGER | Primary Key. Constrained to 1 (singleton). |
| mode | TEXT | Application mode (e.g., `standard`). Default: `standard`. |

### 11. todos

Task tracking within groups.

| Column | Type | Description |
| :--- | :--- | :--- |
| id | INTEGER | Part of Primary Key. Task ID. |
| group_name | TEXT | Part of Primary Key. Group name. |
| description | TEXT | Task description. |
| status | TEXT | Task status (`Open` or `Complete`). Default: `Open`. |

### 12. test_memos

Test table for memo functionality.

| Column | Type | Description |
| :--- | :--- | :--- |
| content | TEXT | Memo content. |
| tags | TEXT | Associated tags. |

## Raw SQL Schema

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
    parsing_strategy TEXT,
    tokens INTEGER DEFAULT 0
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
    accordion_retain_groups INTEGER NOT NULL DEFAULT 2,
    accordion_watermark_tokens INTEGER NOT NULL DEFAULT 350000,
    accordion_epoch_start_group_id TEXT,
    active_skills TEXT,
    parent_session_id TEXT,
    depth INTEGER DEFAULT 0,
    is_ephemeral INTEGER DEFAULT 0
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

CREATE TABLE IF NOT EXISTS scratchpads (
    session_id TEXT PRIMARY KEY,
    content TEXT NOT NULL DEFAULT '',
    updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY(session_id) REFERENCES sessions(id) ON DELETE CASCADE
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

CREATE TABLE IF NOT EXISTS staging_branches (
    branch_name TEXT PRIMARY KEY,
    parent_branch TEXT NOT NULL,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS settings (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    mode TEXT NOT NULL DEFAULT 'standard'
);

CREATE TABLE IF NOT EXISTS todos (
    id INTEGER NOT NULL,
    group_name TEXT,
    description TEXT,
    status TEXT CHECK(status IN ('Open', 'Complete')) DEFAULT 'Open',
    PRIMARY KEY (id, group_name)
);

CREATE TABLE IF NOT EXISTS test_memos (
    content TEXT,
    tags TEXT
);
```

