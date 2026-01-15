# Attempt1 Session Architecture

Sessions are implemented as a partitioned ledger in SQLite. Every interaction is tagged with a session_id.

## Conversation Isolation
Sessions provide isolation of history.

- Mechanism: The messages table includes a session_id for every entry.
- Prompt Construction: The Orchestrator queries only messages associated with the active session_id where status is not 'dropped'.
- Result: The LLM has no visibility into other sessions.

## Shared Global State
Capabilities and knowledge are shared across sessions:

| Feature | Scope |
| :--- | :--- |
| Tools | Global |
| Skills | Global |
| FTS5 Index | Global |

## Mechanics

### Switching Sessions
The /switch [ID] command updates the internal session pointer. The next prompt reconstruction uses history from the new ID.

### Persistence
The ledger is stored in sentinel.db and persists across restarts. Resume a session by providing its ID at startup.

## Summary

| Feature | Isolated per Session? |
| :--- | :--- |
| Message History | Yes |
| LLM Context Window| Yes |
| Tool Registry | No |
| Skills Registry | No |
| FTS5 Search Index | No |