# std::slop Session Architecture

Sessions are implemented as a partitioned ledger in SQLite. Every interaction is tagged with a session_id.

## Conversation Isolation
Sessions provide isolation of history.

- **Mechanism**: The `messages` table includes a `session_id` for every entry.
- **Prompt Construction**: The `Orchestrator` queries only messages associated with the active `session_id` where status is not 'dropped'.
- **Result**: The LLM has no visibility into other sessions.

## Cross-Model Persistence
Sessions are designed to be resilient across model switches. However, because different providers (like Google and OpenAI) use incompatible tool-calling schemas, `std::slop` implements **tool call isolation**.

- **Conversational Text**: Regular user and assistant messages are preserved and automatically re-parsed when you switch models.
- **Tool Isolation**: Tool calls and their results are scoped to the provider strategy that created them. If you switch from Gemini to an OpenAI model, the OpenAI model will see the previous conversation text but will *not* see the Gemini-specific tool calls or results. This prevents parsing errors and "hallucinations" caused by cross-provider format mismatches.

## Shared & Preserved State

While history is isolated, certain configurations are global or preserved in memory when switching.

### Persistence Comparison

| Feature | Scope | Persistence |
| :--- | :--- | :--- |
| **Message History** | Session | SQLite (`messages` table) |
| **Global Anchor (State)** | Session | SQLite (`session_state` table) |
| **Active Scratchpad** | Session | SQLite (`sessions` table) |
| **Context Window Size** | Session | SQLite (`sessions` table) |

**Note**: Both **Global Anchor (State)** and **Active Scratchpad** are persistent across history pruning and session restarts. The Scratchpad is designed to be the session's "source of truth" and can be manually edited by the user to refine the roadmap or redirect the agent.
| **Active Skills** | Process | In-memory (Preserved on `/session`) |
| **Request Throttle** | Process | In-memory (Preserved on `/session`) |
| **Tool Registry** | Global | SQLite (`tools` table) |
| **Skills Registry** | Global | SQLite (`skills` table) |

## Mechanics

### Switching Sessions
The `/session switch <name>` command updates the internal session pointer. 
- **Creation**: If the session name does not exist, it will be implicitly created upon the first message sent to the LLM (when the first record is written to the ledger).
- **What Changes**: The history retrieved for prompt assembly and the session-specific "Global Anchor" state.
- **What Stays**: Your currently activated skills and any `/throttle` settings. This allows you to quickly pivot to a new "thread" or project without re-configuring your preferred persona or agentic behavior.

### Listing Sessions
Use `/session list` to see all sessions that have stored history.

### Starting fresh
To completely clear your context for a new task, simply `/session switch` to a new name (e.g., `/session switch project_part_2`). This is the recommended way to start fresh.

Alternatively, you can use `/session clear` to wipe the current session's history while remaining in that session.

### Removing Sessions
The `/session remove <name>` command permanently deletes a session and all its associated data (history, token usage stats, persistent state, and context settings).
- If the current active session is removed, the system automatically switches to `default_session`.

### Cloning Sessions
The `/session clone <name>` command creates a complete "branch" of the current session.
- **What is copied**: All message history, scratchpad content, persistent state, and token usage history.
- **Uniqueness**: The target name must not already exist.
- **Use Case**: This is ideal for exploring different "branches" of a task or saving a stable state before a risky operation. After cloning, you are automatically switched to the new session.

### Clearing current Session
The `/session clear` command deletes all data (history, token usage stats, persistent state) for the current session and rebuilds the context (making it empty). This is useful if you want to restart a task without changing the session name.

### Persistence
The ledger is stored in `slop.db` and persists across restarts. Resume a session by providing its name at startup or via `/session`.

### Sessions in Batch Mode
When running in Batch Mode (`--prompt`), `std::slop` will use the provided `--session` (or `default_session` if none specified) to retrieve context and store the new interaction. This allows for automated "updates" to a persistent project context.

## Summary

| Feature | Isolated per Session? |
| :--- | :--- |
| Message History | Yes |
| LLM Context Window | Yes |
| Global Anchor State | Yes |
| Active Scratchpad | Yes |
| Tool Registry | No |
| Skills Registry | No |
| Active Skills | No (Preserved on switch) |
| Request Throttle | No (Preserved on switch) |
| FTS5 Search Index | No |
