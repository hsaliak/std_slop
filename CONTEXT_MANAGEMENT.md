# Context Management in std_slop

This document outlines the context management strategy in `std_slop`. We focus on a **Sequential Rolling Window** complemented by **Self-Managed State Tracking**.

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
    - **Memory Loss**: Older context falls off the window. This is mitigated by the **State Tracking** described below.

## 2. Self-Managed State Tracking (Long-term RAM)

To prevent the loss of critical technical details when messages age out of the rolling window, `std_slop` implements a self-managed state tracking mechanism. This allows the LLM to maintain a persistent "Global Anchor" of technical truth.

### The "Context Layers" Approach

When building the prompt, the Orchestrator assembles multiple layers of context:

1.  **System Prompt**: The hard-coded base instructions for the assistant.
2.  **History Guidelines**: Instructions for the LLM on how to interpret the history and the requirement to maintain the state block.
3.  **Global Anchor (`---STATE---`)**: The persistent state blob retrieved from the `session_state` table.
4.  **Conversation History**: The sequential messages retrieved via the rolling window.

### State Persistence and Extraction

The state is managed autonomously by the LLM:
-   **Extraction**: At the end of every response, the LLM is required to include a `---STATE---` block. The `Orchestrator` parses this block from the response and saves it to the `session_state` table in the database.
-   **Injection**: Before every new prompt, the `Orchestrator` retrieves the latest state for the session and injects it as the "Global Anchor."

This creates a "Long-term RAM" that is actively rewritten and curated by the LLM itself, ensuring that critical information (active files, technical anchors, resolved issues) is preserved even if the original messages that defined them have been truncated.

---STATE---
Goal: [Short description of current task]
Context: [Active files/classes being edited]
Resolved: [List of things finished this session]
Technical Anchors: [Ports, IPs, constant values]
---END STATE---

## Evolution: Why we removed FTS-Ranked Mode

Earlier versions of `std_slop` included a `FTS_RANKED` mode that used hybrid retrieval (BM25 + Recency) via SQLite FTS5. While theoretically powerful for long sessions, it was removed for the following reasons:

1.  **Stop-Word Pollution**: Common conversational phrases like "continue," "next," or "go on" acted as high-relevance search terms. This caused the system to pull in random historical fragments where those words appeared, filling the context window with irrelevant noise.
2.  **Narrative Fragmentation**: Non-sequential retrieval often confused the LLM. If the "middle" of a technical implementation was missing because it didn't match the current keyword, the LLM would hallucinate missing details or repeat work.
3.  **Complexity vs. Value**: Addressing the noise would have required complex stop-word filtering and query expansion logic. Instead, we chose to **simplify**. By focusing on a sequential window and a robust, LLM-managed `---STATE---` block, we ensure that critical "technical anchors" are preserved without the unpredictability of keyword-based retrieval.

The current strategy prioritizes **coherence** (sequential history) and **authoritative summary** (the state block) over autonomous retrieval.

## Commands

- `/context window <N>`: Set the size of the rolling window (number of interaction groups). Use 0 for full history.
- `/context show`: Display the exact assembled context that will be sent to the LLM.
- `/context rebuild`: Rebuilds the session state (---STATE--- anchor) from the current context window history. Use this if the state becomes corrupted or out of sync.
- `/message remove <GID>`: Permanently **deletes** a specific message group from the database.
