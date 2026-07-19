# std::slop Session Architecture
Sessions are implemented as a partitioned ledger in SQLite. Every interaction is tagged with a session_id.
## Conversation Isolation
Sessions provide isolation of history.
- **Mechanism**: The `messages` table includes a `session_id` for every entry.
- **Prompt Construction**: The `Orchestrator` queries only messages associated with the active `session_id` where status is not 'dropped'.
- **Result**: The LLM has no visibility into other sessions.
## Model Changes
Session history is retained when the configured model changes. Provider response normalization is handled by the Responses orchestrator.
## Shared & Preserved State
While history is isolated, certain configurations are global or preserved in memory when switching.
### Persistence Comparison
| Feature | Scope | Persistence |
| :--- | :--- | :--- |
| **Message History** | Session | SQLite (`messages` table) |
| **Context Window Size** | Session | SQLite (`sessions` table) |
| **Active Skills** | Process | In-memory (Preserved on `/session`) |
| **Request Throttle** | Process | In-memory (Preserved on `/session`) |
| **Tool Registry** | Global | SQLite (`tools` table) |
| **Skills Registry** | Global | SQLite (`skills` table) |
## Mechanics
### Switching Sessions
The `/session switch <name>` command updates the internal session pointer.
- **Creation**: If the session name does not exist, it will be implicitly created upon the first message sent to the LLM (when the first record is written to the ledger).
- **What Changes**: The history retrieved for prompt assembly.
- **What Stays**: Your currently activated skills and any `/throttle` settings. This allows you to quickly pivot to a new "thread" or project without re-configuring your preferred persona or agentic behavior.
### Listing Sessions
Use `/session list` to see all sessions that have stored history.
### Starting fresh
To completely clear your context for a new task, simply `/session switch` to a new name (e.g., `/session switch project_part_2`). This is the recommended way to start fresh.
Alternatively, you can use `/session clear` to wipe the current session's history while remaining in that session.
### Removing Sessions
The `/session remove <name>` command permanently deletes a session and all its associated data (history, token usage stats, and context settings).
- If the current active session is removed, the system automatically switches to `default_session`.
### Cloning Sessions
The `/session clone <name>` command creates a complete "branch" of the current session.
- **What is copied**: All message history and token usage history.
- **Uniqueness**: The target name must not already exist.
- **Use Case**: This is suitable for exploring different "branches" of a task or saving a stable state before a complex change. After cloning, you are automatically switched to the new session.
### Clearing current Session
The `/session clear` command deletes all data (history and token usage stats) for the current session. This is useful if you want to restart a task without changing the session name.
### Persistence
The ledger is stored in `slop.db` and persists across restarts. Resume a session by providing its name at startup or via `/session`.
### Sessions in Batch Mode
Batch mode accepts `--prompt` or `--prompt_file` and uses `--session` or `default_session`. It uses an in-memory database unless `--prompt_db` is set.

`--output=json` writes run metadata. `--format` or `--format_file` requests schema-constrained output and writes the validated JSON value to stdout; it cannot be combined with `--output=json`. See the root [README](../README.md#batch-mode) for examples and the supported schema subset.
## Summary
| Feature | Isolated per Session? |
| :--- | :--- |
| Message History | Yes |
| LLM Context Window | Yes |
| Tool Registry | No |
| Skills Registry | No |
| Active Skills | No (Preserved on switch) |
| Request Throttle | No (Preserved on switch) |
| FTS5 Search Index | No |

