# std::slop Session Architecture

Sessions are implemented as a partitioned ledger in SQLite. Every interaction is tagged with a session_id.

## Conversation Isolation
Sessions provide isolation of history.

- **Mechanism**: The `messages` table includes a `session_id` for every entry.
- **Prompt Construction**: The `Orchestrator` queries only messages associated with the active `session_id` where status is not 'dropped'.
- **Result**: The LLM has no visibility into other sessions.

## Shared & Preserved State

While history is isolated, certain configurations are global or preserved in memory when switching.

### Persistence Comparison

| Feature | Scope | Persistence |
| :--- | :--- | :--- |
| **Message History** | Session | SQLite (`messages` table) |
| **Global Anchor (State)** | Session | SQLite (`session_state` table) |
| **Context Window Size** | Session | SQLite (`sessions` table) |
| **Active Skills** | Process | In-memory (Preserved on `/session`) |
| **Request Throttle** | Process | In-memory (Preserved on `/session`) |
| **Tool Registry** | Global | SQLite (`tools` table) |
| **Skills Registry** | Global | SQLite (`skills` table) |

## Mechanics

### Switching Sessions
The `/session [name]` command updates the internal session pointer. 
- **Creation**: If the session name does not exist, it will be implicitly created upon the first message sent to the LLM (when the first record is written to the ledger).
- **What Changes**: The history retrieved for prompt assembly and the session-specific "Global Anchor" state.
- **What Stays**: Your currently activated skills and any `/throttle` settings. This allows you to quickly pivot to a new "thread" or project without re-configuring your preferred persona or agentic behavior.

### Starting fresh
To completely clear your context for a new task, simply `/session` to a new name (e.g., `/session project_part_2`). This is the recommended way to start fresh.

### Persistence
The ledger is stored in `slop.db` and persists across restarts. Resume a session by providing its name at startup or via `/session`.

## Summary

| Feature | Isolated per Session? |
| :--- | :--- |
| Message History | Yes |
| LLM Context Window | Yes |
| Global Anchor State | Yes |
| Tool Registry | No |
| Skills Registry | No |
| Active Skills | No (Preserved on switch) |
| Request Throttle | No (Preserved on switch) |
| FTS5 Search Index | No |
