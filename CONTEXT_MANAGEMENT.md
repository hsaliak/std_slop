# Context Management in std::slop

This document outlines the context management strategy in `std::slop`. We focus on a **Sequential Rolling Window** complemented by **Self-Managed State Tracking** and **On-Demand Historical Retrieval**.

The system groups messages into "conversation groups" (identified by `group_id`) to maintain logical coherence (e.g., a user prompt and its resulting tool calls and assistant response form a group).

## 1. Sequential Rolling Window

The system treats the conversation history as a linear timeline. This ensures that the narrative flow and the sequence of technical operations are preserved.

### Mechanism
- **Retrieval**: Fetches the conversation history for the session.
- **Selection**:
    - If a `context_size` limit (N) is set, it identifies the last N distinct `group_id`s in the history.
    - It then filters the history to include only messages belonging to these last N groups.
    - A `context_size` of 0 indicates "Full History" (no windowing).
- **Ordering**: Strict chronological order.

### Tradeoffs
- **Pros**:
    - **Coherence**: Guarantees the most recent conversation flow is preserved intact.
    - **Simplicity**: Easy to reason about; "what you see is what you get" (the last N exchanges).
    - **Reliability**: Avoids the "hallucination" or confusion that can occur with non-sequential fragments.
- **Cons**:
    - **Memory Loss**: Older context falls off the window. This is mitigated by **State Tracking** and **Historical Retrieval**.

## 2. Multi-Strategy Orchestration and Tool Call Isolation

As sessions evolve, users might switch between different LLM providers (e.g., shifting from Gemini to OpenAI). Different providers often use incompatible formats for tool calls and message structures. To ensure stability and prevent parsing errors, `std::slop` implements **Tool Call Isolation**.

### Strategy Tagging
Every message appended to the database is tagged with the `parsing_strategy` that was active when it was created. Common strategies include `openai`, `gemini`, and `gemini_gca`.

### Strategy-Aware History Filtering
When the `Orchestrator` assembles a prompt, it filters the historical messages based on the currently active strategy:

1.  **Text Messages**: User and Assistant messages containing only text are preserved across model switches. They are automatically re-parsed into the target model's format.
2.  **Tool Isolation**: Messages with a `role` of `tool` or a `status` of `tool_call` are only included if their `parsing_strategy` matches the currently active one (with compatibility between `gemini` and `gemini_gca`).
3.  **Rationale**: Providers (like Google and OpenAI) use vastly different JSON schemas and sequences for tool interactions. Attempting to "translate" a complex tool chain from one provider to another often leads to hallucinations or API errors. Isolation ensures that the LLM only sees tool interactions it is capable of understanding and continuing.

This approach balances cross-model conversational continuity with the strict technical requirements of tool-calling APIs. Information that must persist across tool-isolated boundaries should be recorded in the **Global Anchor (State)**.

### Centralized Message Parsing
To handle the divergent JSON schemas between providers, `std::slop` uses a centralized `MessageParser` class (`core/message_parser.h`). This utility:
- **Extracts Tool Calls**: Parses tool call JSON from both OpenAI (`tool_calls` array) and Gemini (`functionCall` object) formats.
- **Extracts Assistant Text**: Retrieves the text content from tool_call messages for display purposes.
- **Strategy-Aware**: Uses the `parsing_strategy` field to determine the correct parsing logic.

This centralization eliminates heuristic JSON sniffing and ensures consistent tool call extraction across the UI and orchestrator components.

### Dynamic Tool Truncation
To maximize context efficiency, the system applies differential truncation limits to tool results based on their recency and group status. These limits are configurable via the `TruncationSettings` struct.

- **Active Group (Recent - Full Fidelity)**: The **5 most recent** tool results in the current conversation group are kept at up to **5000 characters**. This ensures the LLM has full context for immediate, iterative tool loops.
- **Active Group (Degraded)**: Any tool results in the active group beyond the 5 most recent are truncated to **1000 characters**. This prevents a single long-running turn with many tool calls from consuming the entire context window.
- **Inactive Groups (Compression)**: Once a conversation group becomes inactive (the turn has finished and a new user prompt has started), all tool results in that group are truncated to **300 characters**.
- **Retrieval Hints**: All truncated results are appended with a hint: `... [TRUNCATED. Use query_db(sql="SELECT content FROM messages WHERE id=<ID>") to see full output]`. This allows the model to retrieve full technical detail on-demand via SQL.del to surgically retrieve full technical detail from its own history if a previous task needs re-investigation.

**Rationale**: The specific details of a tool's output are critically important *while* the task is being performed. However, once the task is complete, the *essence* of the result is usually sufficient. Reducing the limit to 300 characters while providing an explicit recovery path (via `query_db`) offers the best balance of context efficiency and technical depth.

## 3. Self-Managed State Tracking (Long-term RAM)

To prevent the loss of critical technical details when messages age out of the rolling window, `std::slop` implements a self-managed state tracking mechanism. This allows the LLM to maintain a persistent "Global Anchor" of technical truth.

### The "Context Layers" Approach

When building the prompt, the Orchestrator assembles multiple layers of context:

1.  **System Prompt**: The hard-coded base instructions for the assistant.
2.  **History Guidelines**: Instructions for the LLM on how to interpret the history and the requirement to maintain the state block.
3.  **Global Anchor (`### STATE`)**: The persistent state blob retrieved from the `session_state` table.
4.  **The Scratchpad**: A flexible, persistent workspace for evolving plans and task tracking.
5.  **Conversation History**: The sequential messages retrieved via the rolling window.

### State Persistence and Extraction

The state is managed autonomously by the LLM:
-   **Extraction**: At the end of every response, the LLM is required to include a `### STATE` block. The `Orchestrator` parses this block from the response and saves it to the `session_state` table in the database.
-   **Injection**: Before every new prompt, the `Orchestrator` retrieves the latest state for the session and injects it as the "Global Anchor."

This creates a "Long-term RAM" that is actively rewritten and curated by the LLM itself, ensuring that critical information (active files, technical anchors, resolved issues) is preserved even if the original messages that defined them have been truncated.

### State Format
```markdown
### STATE
Goal: [Short description of current task]
Context: [Active files/classes being edited]
Resolved: [List of things finished this session]
Technical Anchors: [Ports, IPs, constant values]
```

## 4. The Scratchpad: Evolutionary Planning

As the system evolved, we replaced the structured, rigid `/todo` table with a more flexible **Scratchpad**. This shift represents a move from human-managed task lists to LLM-managed architectural and implementation plans.

### Flexible Workspace
- **Evolution**: Unlike the fixed schema of the `todos` table, the scratchpad is a raw text/markdown field in the `sessions` table. It allows for hierarchical plans, notes, and evolving implementation details that don't fit into a simple "Open/Complete" status.
- **Persistence**: The scratchpad is specific to the session and persists across model switches and session restarts.
- **LLM Introspection**: The LLM is equipped with tools (`manage_scratchpad`) to read, update, and append to the scratchpad. This allows it to "think out loud" or maintain a checklist that stays in context even as messages roll off the window.

### User Transparency
The scratchpad functionality is designed to be transparent. While the user can interact with it via `/session scratchpad`, the primary use case is for the LLM to autonomously maintain its own plan. The user simply ensures the plan is initialized or updated by asking the LLM to "update the scratchpad" or "save the plan to the scratchpad."

## 5. Historical Context Retrieval (SQL-based Retrieval)

Unique to `std::slop`, the agent has the capability to query its own message history directly via SQL when the rolling window is insufficient.

### Mechanism
The agent is instructed to use the `query_db` tool to search the `messages` table. This allows for precision retrieval of old information that has fallen out of the rolling window without bloating the context with irrelevant data.

### Guidelines for the Agent
- **Recency Bias**: Queries should generally use `ORDER BY id DESC` to find the most relevant recent information.
- **Data Integrity**: The agent must ignore records where `status = 'dropped'` to avoid retrieving "undone" or invalid history.
- **Selective Retrieval**: The agent can search by `role`, `content` keywords, or `group_id`.

Example query the agent might use:
```sql
SELECT role, content FROM messages 
WHERE status != 'dropped' AND content LIKE '%refactor plan%' 
ORDER BY id DESC LIMIT 5;
```

## 6. Manual Context Intervention

Users have several tools to manually prune or repair the context:

### The `/undo` Command
The `/undo` command is a high-level shortcut for:
1. Identifying the most recent message group (`group_id`).
2. Deleting all messages in that group from the database.
3. Triggering a context rebuild.

This is the primary way to "roll back" an interaction that went wrong or to retry a prompt with different instructions.

### The `/message remove <GID>` Command
For more granular control, users can remove any specific message group by its ID.

## 7. Semantic Memo System

Beyond session-specific state and rolling history, `std::slop` provides a **Semantic Memo System** for long-term knowledge persistence across sessions and projects.

### Purpose
Memos are designed for information that is:
- **High-Value**: Architectural decisions, non-obvious "gotchas," or complex API designs.
- **Persistent**: Information that should remain available even if the original conversation is deleted or archived.
- **Discoverable**: Tagged semantically for easy retrieval by the LLM during future tasks.

### Mechanism
- **Creation**: Memos are created using the `save_memo` tool. Each memo consists of content and a set of semantic tags (e.g., `arch-decision`, `gotcha`, `api-design`).
- **Retrieval**:
    - **Automatic**: The orchestrator extracts keywords from user prompts and automatically injects up to 5 relevant memos into the system instructions.
    - **Manual/Explicit**: The LLM can also use the `retrieve_memos` tool to find specific information based on tags.
- **Persistence**: Memos are stored in the `llm_memos` table and are independent of any specific session.

### Strategic Usage
The system prompt instructs the LLM to:
1. **Check Memos**: Look for existing knowledge before making assumptions about architectural patterns or system behavior.
2. **Record Knowledge**: Save new memos when a significant discovery is made or an important decision is finalized.
3. **Avoid Redundancy**: Only save memos for information that is NOT easily discoverable in the codebase itself (e.g., the "why" behind a design).

### The `/context drop <GID>` Command
For more surgical context management, users can drop a specific message group without deleting it. This is useful for removing "noise" from the middle of a history that might be confusing the LLM.

### The `/context rebuild` Command
Since the `### STATE` block is derived from the *last* assistant message, removing messages can leave the persistent state out of sync with the now-current history. `/context rebuild` asks the LLM to look at the *entire current window* and generate a new, accurate `### STATE` block.

## Evolution: Why we removed FTS-Ranked Mode

Earlier versions of `std::slop` included a `FTS_RANKED` mode that used hybrid retrieval (BM25 + Recency) via SQLite FTS5. While theoretically powerful for long sessions, it was removed for the following reasons:

1.  **Stop-Word Pollution**: Common conversational phrases like "continue," "next," or "go on" acted as high-relevance search terms. This caused the system to pull in random historical fragments where those words appeared, filling the context window with irrelevant noise.
2.  **Narrative Fragmentation**: Non-sequential retrieval often confused the LLM. If the "middle" of a technical implementation was missing because it didn't match the current keyword, the LLM would hallucinate missing details or repeat work.
3.  **Complexity vs. Value**: Addressing the noise would have required complex stop-word filtering and query expansion logic. Instead, we chose to **simplify**. By focusing on a sequential window and a robust, LLM-managed `### STATE` block, we ensure that critical "technical anchors" are preserved without the unpredictability of keyword-based retrieval.

The current strategy prioritizes **coherence** (sequential history) and **authoritative summary** (the state block) while allowing for **agent-driven precision retrieval** via SQL.

## Token Accounting

Token counts in `std::slop` (displayed as `· NNN tokens`) do not represent the isolated cost of a single message, but rather a **snapshot of the session's total weight** in the LLM's memory at that specific moment.

### The Snapshot Principle
Every interaction with the LLM is stateless. To provide context, the orchestrator sends the entire relevant conversation history back to the model with every new request. The reported token count is the sum of:
1.  **Prompt Tokens**: The "Input" (System Instructions + Scratchpad + Conversation History + New User Message).
2.  **Completion Tokens**: The "Output" (The LLM's new reasoning text and/or tool calls).

### Behavioral Characteristics
*   **Growth**: As long as the session history is preserved, the token count will strictly increase turn-by-turn as the history "Prompt Tokens" grows.
*   **Intra-Turn Consistency**: In a single turn containing multiple tool calls (Parallel Tool Calling), every tool call and assistant message will display the **same** token count. This represents the weight of the *entire interaction* that produced those calls.
*   **Pruning Discontinuity**: The token count will only decrease when the **Rolling Context Window** prunes old messages. If a large historical block is dropped to fit the window limit, the next turn's count will drop accordingly.

## Commands Reference

- `/context window <N>`: Set the size of the rolling window (number of interaction groups). Use 0 for full history.
- `/context show`: Display the exact assembled context that will be sent to the LLM. The output is human-readable and will automatically open in your `$EDITOR` (e.g., `vim`, `nano`) if it exceeds terminal height.
- `/context rebuild`: Rebuilds the session state (`### STATE` anchor) from the current context window history.
- `/undo`: Shortcut to remove the last interaction and rebuild state.
- `/message list [N]`: List the last `N` interaction groups with token usage information.
- `/message show <GID>`: View the full content of a specific interaction group.
- `/message remove <GID>`: Permanently **deletes** a specific message group from the database.
- `/messages`: Alias for the `/message` command.

## Evolutionary Notes

### 2026-01-23: Historical Truncation vs. Retrieval
**Change**: Reduced historical truncation limit from 500 to 300 characters and implemented `query_db` hints.
**Rationale**: Deepening the isolation and efficiency of the context window. By providing a programmatic retrieval hint, the model can safely operate with much smaller historical fragments, as it knows exactly how to get the full data if it becomes relevant again. This significantly extends the effective session length for complex engineering tasks.

### 2026-01-27: Intra-Turn Degradation
**Change**: Implemented a 3-tier truncation strategy: Full-fidelity (5000 chars) for the last 5 tool calls in the active group, Degraded (1000 chars) for older calls in the active group, and Inactive (300 chars) for previous turns.
**Rationale**: Complex tasks often involve many tool calls within a single "turn" (e.g., recursive file searches or broad refactors). Preserving 5000 characters for *every* call in a 20-call sequence would instantly exhaust the context window. By degrading older calls within the same turn to 1000 characters, we balance technical fidelity for the current task with the need to preserve conversation history and the Global Anchor state.
