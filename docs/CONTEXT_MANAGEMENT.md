# Context Management in std::slop
`std::slop` uses append-only accordion history. Messages are grouped by `group_id`, so a user request, its tool calls, and its final assistant response remain together.
## 1. Assistant History State
The system prompt asks the assistant to include a `### STATE` summary in its responses. That summary remains in the corresponding assistant message in conversation history and is the only state supplied to later model requests. std::slop does not extract, validate, or duplicate the summary into a separate system-prompt anchor; if the assistant omits it, the history simply has no such summary for that turn.

1.  **Text Messages**: User and Assistant messages containing only text are preserved across model switches. They are automatically re-parsed into the target model's format.
2.  **Tool Isolation**: Messages with a `role` of `tool` or a `status` of `tool_call` are only included if their `parsing_strategy` matches the currently active one.
3.  **Rationale**: Providers (like Google and OpenAI) use vastly different JSON schemas and sequences for tool interactions. Attempting to "translate" a complex tool chain from one provider to another often leads to hallucinations or API errors. Isolation ensures that the LLM only sees tool interactions it is capable of understanding and continuing.
This approach balances cross-model conversational continuity with the strict technical requirements of tool-calling APIs.
### Centralized Message Parsing
To handle the divergent JSON schemas between providers, `std::slop` uses a centralized `MessageParser` class (`core/message_parser.h`). This utility:
- **Extracts Tool Calls**: Parses tool call JSON from the OpenAI Responses API format.
- **Extracts Assistant Text**: Retrieves the text content from tool_call messages for display purposes.
- **Strategy-Aware**: Uses the `parsing_strategy` field to determine the correct parsing logic.
This centralization eliminates heuristic JSON sniffing and ensures consistent tool call extraction across the UI and orchestrator components.
### Dynamic Tool Truncation
To maximize context efficiency, the system applies differential truncation limits to tool results based on their recency and group status. These limits are configurable via the `TruncationSettings` struct.
- **Active Group (Recent - Full Fidelity)**: The **5 most recent** tool results in the current conversation group are kept at up to **5000 characters**. This ensures the LLM has full context for immediate, iterative tool loops.
- **Active Group (Degraded)**: Any tool results in the active group beyond the 5 most recent are truncated to **1000 characters**. This prevents a single long-running turn with many tool calls from consuming the entire context window.
- **Inactive Groups (Compression)**: Once a conversation group becomes inactive (the turn has finished and a new user prompt has started), all tool results in that group are truncated to **300 characters**.
- **Retrieval Hints**: All truncated results are appended with a hint: `... [TRUNCATED. Use query_db(sql="SELECT content FROM messages WHERE id=<ID>") to see full output]`. This allows the model to retrieve full technical detail on-demand via SQL.del to surgically retrieve full technical detail from its own history if a previous task needs re-investigation.
**Rationale**: The specific details of a tool's output are essential *while* the task is being performed. However, once the task is complete, the *essence* of the result is usually sufficient. Reducing the limit to 300 characters while providing an explicit recovery path (via `query_db`) offers the optimal trade-off of context efficiency and technical depth.
## 3. Assistant-Managed State Tracking
The system prompt allows the assistant to summarize technical progress in its response history. The current accordion epoch determines how long those summaries remain available to later requests.
### The "Context Layers" Approach
When building the prompt, the Orchestrator assembles multiple layers of context:
1.  **System Prompt**: The hard-coded base instructions for the assistant.
2.  **History Guidelines**: Instructions for the LLM on how to interpret optional state blocks in history.
3.  **Assistant State (`### STATE`)**: A summary retained in the assistant message that produced it.
4.  **Conversation History**: The sequential messages retrieved via the rolling window.
### Optional State Summaries
The assistant may manage state summaries autonomously:
-   **History**: The system prompt allows the assistant to include a `### STATE` block. When present, it remains in the assistant response in conversation history.
The assistant-managed summary is not extracted or injected separately. It is available only while the assistant message containing it remains in the selected accordion history.
### Optional State Format
```markdown
### STATE
Goal: [Short description of current task]
Context: [Active files/classes being edited]
Resolved: [List of things finished this session]
Technical Anchors: [Ports, IPs, constant values]
```
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
3. Rebuilding the next prompt from the remaining history.
This is the primary way to "roll back" an interaction that went wrong or to retry a prompt with different instructions.
### The `/message remove <GID>` Command
For more granular control, users can remove any specific message group by its ID.
## Evolution: Why we removed FTS-Ranked Mode
Earlier versions of `std::slop` included a `FTS_RANKED` mode that used hybrid retrieval (BM25 + Recency) via SQLite FTS5. While theoretically robust for long sessions, it was removed for the following reasons:
1.  **Stop-Word Pollution**: Common conversational phrases like "continue," "next," or "go on" acted as high-relevance search terms. This caused the system to pull in random historical fragments where those words appeared, filling the context window with irrelevant noise.
2.  **Narrative Fragmentation**: Non-sequential retrieval often confused the LLM. If the "middle" of a technical implementation was missing because it didn't match the current keyword, the LLM would hallucinate missing details or repeat work.
3.  **Complexity vs. Value**: Addressing the noise would have required complex stop-word filtering and query expansion logic. Instead, we chose to **simplify**. By focusing on a sequential window and a robust, LLM-managed `### STATE` block, we ensure that critical "technical anchors" are preserved without the unpredictability of keyword-based retrieval.
The current strategy prioritizes **coherence** (sequential history) and **authoritative summary** (the state block) while allowing for **agent-driven precision retrieval** via SQL.
## Token Accounting
Token counts in `std::slop` (displayed as `· NNN tokens`) do not represent the isolated cost of a single message, but rather a **snapshot of the session's total weight** in the LLM's memory at that specific moment.
### The Snapshot Principle
Every interaction is stateless. The orchestrator sends the selected accordion history with each request. The reported token count is the sum of:
1.  **Prompt Tokens**: The "Input" (System Instructions + Conversation History + New User Message).
2.  **Completion Tokens**: The "Output" (The LLM's new reasoning text and/or tool calls).
### Behavioral Characteristics
*   **Growth**: As long as the session history is preserved, the token count will strictly increase turn-by-turn as the history "Prompt Tokens" grows.
*   **Intra-Turn Consistency**: In a single turn containing multiple tool calls (Parallel Tool Calling), every tool call and assistant message will display the **same** token count. This represents the weight of the *entire interaction* that produced those calls.
*   **Accordion Reset**: Context is append-only within an epoch. When the latest successful provider prompt reaches the high watermark, the next generation resets history to the configured complete-group suffix and begins a new epoch.
## Commands Reference
- `/context <N> [watermark]`: Enable accordion context. `N` is the number of most-recent interaction groups retained at reset; the watermark defaults to 350000 actual prompt tokens.
- `/context show`: Display accordion settings and the exact assembled context that will be sent to the LLM. The output is human-readable and will automatically open in your `$EDITOR` (e.g., `vim`, `nano`) if it exceeds terminal height.
- If a provider still rejects context length, std::slop resets once and retries generation once. Set the watermark near 80% of known model capacity or reduce `N` if that retry also fails.
- `/undo`: Shortcut to remove the last interaction.
- `/message list [N]`: List the last `N` interaction groups with token usage information.
- `/message show <GID>`: View the full content of a specific interaction group.
- `/message remove <GID>`: Permanently **deletes** a specific message group from the database.
- `/messages`: Alias for the `/message` command.
