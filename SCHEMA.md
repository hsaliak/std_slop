# std::slop Database Schema

This document describes the SQLite schema used by std::slop to persist history, tools, skills, indexed code, and usage statistics.

## Tables

### 1. messages
Stores user prompts, assistant responses, and tool executions.

| Column | Type | Description |
| :--- | :--- | :--- |
| id | INTEGER | Primary Key. |
| session_id | TEXT | Conversation identifier. |
| role | TEXT | `system`, `user`, `assistant`, or `tool`. |
| content | TEXT | Message text or tool JSON. |
| tool_call_id | TEXT | Metadata for linking responses. |
| status | TEXT | `completed`, `tool_call`, or `dropped`. |
| group_id | TEXT | Turn identifier for atomic operations (usually Unix nanoseconds). |
| created_at | DATETIME | Entry timestamp. |

### 2. tools
Registry of available agent tools.

| Column | Type | Description |
| :--- | :--- | :--- |
| name | TEXT | Primary Key. Unique tool name. |
| description | TEXT | Functional description. |
| json_schema | TEXT | Argument definition in JSON Schema format. |
| is_enabled | INTEGER | Status flag (1 for active, 0 for inactive). |

### 3. skills
Persona patches for specialized instructions.

| Column | Type | Description |
| :--- | :--- | :--- |
| id | INTEGER | Primary Key. |
| name | TEXT | Unique skill name. |
| description | TEXT | Purpose description. |
| system_prompt_patch| TEXT | Instructions to inject into the system prompt. |

### 4. sessions
Persists user settings for each conversation session.

| Column | Type | Description |
| :--- | :--- | :--- |
| id | TEXT | Primary Key. Session ID. |
| context_mode | TEXT | `FULL` or `FTS_RANKED`. |
| context_size | INTEGER | Number of groups to retrieve in ranked mode. |

### 5. usage
Tracks token usage for cost and performance monitoring.

| Column | Type | Description |
| :--- | :--- | :--- |
| id | INTEGER | Primary Key. |
| session_id | TEXT | Associated session. |
| model | TEXT | Model used for the interaction. |
| prompt_tokens | INTEGER | Tokens in the prompt. |
| completion_tokens | INTEGER | Tokens in the response. |
| total_tokens | INTEGER | Sum of prompt and completion tokens. |
| created_at | DATETIME | Timestamp of the interaction. |

### 6. group_search (Virtual Table)

FTS5 table for weighted hybrid RRF context retrieval.

| Column | Type | Description |
| :--- | :--- | :--- |
| group_id | TEXT | Unindexed group identifier. |
| content | TEXT | Concatenated text of all messages in group. |

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
    context_mode TEXT DEFAULT 'FULL',
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

CREATE VIRTUAL TABLE IF NOT EXISTS group_search USING fts5(group_id UNINDEXED, content);
```
