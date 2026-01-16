# Context Management in std_slop

This document outlines the current state of context management in `std_slop`. We employ two primary strategies for managing the LLM's context window: **FULL (Rolling Window)** and **FTS_RANKED (Hybrid Retrieval)**.

The system groups messages into "conversation groups" (identified by `group_id`) to maintain logical coherence (e.g., a user prompt and its resulting tool calls and assistant response form a group).

## 1. FULL Mode (Rolling Window)

The default and simplest mode. It treats the conversation history as a linear timeline.

### Mechanism
- **Retrieval**: Fetches the conversation history for the session.
- **Selection**:
    - If a `context_size` limit (N) is set, it identifies the last N distinct `group_id`s in the history.
    - It then filters the history to include only messages belonging to these last N groups.
- **Ordering**: Strict chronological order.

### Tradeoffs
- **Pros**:
    - **Coherence**: Guarantees the most recent conversation flow is preserved intact.
    - **Simplicity**: Easy to reason about; "what you see is what you get" (the last N exchanges).
    - **Low Latency**: No complex ranking or search operations required.
- **Cons**:
    - **Memory Loss**: Older, potentially critical context (e.g., a specific instruction given at the start of a long session) falls off the window and is completely forgotten.
    - **Inefficiency**: Wastes context tokens on recent but potentially irrelevant chit-chat while dropping relevant older technical details.

## 2. FTS_RANKED Mode (Hybrid Retrieval via RRF)

A more advanced mode designed to retain relevant information from across the entire session history, regardless of age. It uses **Reciprocal Rank Fusion (RRF)** to combine keyword search relevance with chronological recency.

### Mechanism

1.  **Search (FTS5)**:
    -   The system extracts the content of the *latest user query*.
    -   It queries a virtual SQLite FTS5 table (`group_search`) which indexes all conversation groups.
    -   Returns a list of `group_id`s ranked by BM25 relevance (`fts_ranked`).

2.  **Recency**:
    -   A list of `group_id`s is generated based on reverse chronological order (`recency_ranked`).
    -   **Subset Rule**: If the FTS search returned matches, the recency list is filtered to prioritize recent groups *that were also found in the search*. If no search matches are found, it falls back to standard global recency.

3.  **Ranking (RRF)**:
    -   Scores are calculated for each group present in either list using the RRF formula:
        $$ Score(d) = \sum_{r \in R} \frac{1}{k + r(d)} $$
    -   We use a constant $k=60$.
    -   **Weighting**: We currently weight BM25 scores 1.5x higher than recency scores to bias towards semantic relevance over pure recency.

4.  **Selection**:
    -   Groups are sorted by their final RRF score.
    -   The top N groups (defined by `context_size`) are selected.

5.  **Mandatory Inclusions**:
    -   **Current Group**: The group currently being formed (the user's latest input) is *always* included to ensure the model sees the immediate prompt.
    -   **System Messages**: All messages with the `system` role are preserved to maintain core instructions.

6.  **Re-ordering**:
    -   The final selected set of groups is re-sorted chronologically before being sent to the LLM. This preserves the narrative flow within the constructed context, even if there are gaps.

### Tradeoffs
- **Pros**:
    -   **Recall**: Can pull in relevant context from very early in a long session if it matches the current topic (e.g., recalling a specific file path mentioned 50 turns ago).
    -   **Efficiency**: Optimizes the context window to contain "high-value" information relevant to the immediate query.
- **Cons**:
    -   **Fragmentation**: The context can become "holey." Chronological gaps might confuse the model if it relies on implicit state from missing intermediate steps.
    -   **Complexity**: Harder to debug why a specific piece of context was included or excluded.
    -   **Dependency**: heavily relies on the quality of the search query (the user's last prompt). If the prompt is vague, retrieval quality suffers.

## Current State & Future Work

Currently, `FTS_RANKED` uses a simple BM25 search on the raw message content.
-   **Improvement Area**: The search query is just the raw user prompt. This can be noisy. Generating a dedicated "search query" or summary from the prompt might improve retrieval.
-   **Improvement Area**: The "Subset Rule" for recency might be too aggressive, potentially starving strictly recent context if the search term matches distinct older items.
-   **Improvement Area**: `code_search` was deprecated in favor of tool-based search (grep), but the concept of "semantic search" for code context (embeddings) is a potential future direction.
