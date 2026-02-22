# Lua Control Plane & Orchestration

`std::slop` uses (Lua 5.5) Lua 5.4+ as its high-level orchestration layer and "control plane." Instead of executing single tools in isolation, the agent writes and executes Lua scripts that combine tools, handle complex logic, and perform parallel operations safely.

## 1. The Core Philosophy: Why Lua?

The primary reason for the Lua Control Plane (LCP) is to solve the **Context Rot** problem inherent in large-scale LLM interactions.

### Code as a Scalpel
Traditional agents often ingest raw, massive datasets into their context window (e.g., reading a 2000-line file just to find one function). This leads to "context rot" where the model's reasoning is degraded by irrelevant information.
In `std::slop`, the LCP allows the agent to  pass the context programatically into a sub-query and evaluate the provided results, avoiding rot.
- Instead of reading a whole file, a Lua script can grep for a pattern, process the result in-memory, and only return the relevant snippets.
- Data filtering happens *within* the LCP, not the LLM's context window.

### Parallelism and Efficiency
The LCP supports asynchronous execution. An agent can initiate multiple file reads, code searches, or even sub-LLM queries simultaneously using `_async` tool variants, drastically reducing the latency of complex investigative tasks.

### Persistence and State Continuity
The LCP provides a persistent environment across turns. By using the `scratchpad`, `memos`, and `state` globals, the agent maintains a "Source of Truth" that is programmatically accessible, reducing the need to re-summarize or re-search for the same information in every turn. These globals are saved in sqlite at the end of a turn, and injected into the environment in the next.

---

## 2. The Recursive Language Model (RLM) Paradigm

The LCP implements the **Recursive Language Model (RLM)** paradigm. In this model, the agent processes arbitrarily long contexts by treating the codebase, history, and scratchpad as external variables in a persistent Lua environment.

The LCP acts as the "inner loop" of the agent's cognition:
1.  **Analyze**: The LLM analyzes the current state and goal.
2.  **Orchestrate**: The LLM writes a Lua script to perform the next logical step.
3.  **Execute**: The LCP executes the script, interacting with the filesystem, database, and sub-LLMs.
4.  **Refine**: The results are returned to the LLM to refine the next step.

---

## 3. The Lua Environment

Scripts executed via `run_lua` have access to a rich environment tailored for software engineering.

### Global Symbolic Handles
These variables bridge the gap between individual turns and provide persistent context.

| Global | Purpose | Usage |
| :--- | :--- | :--- |
| `scratchpad` | Working notes and checklists. | Read at turn start; update via `tools.manage_scratchpad`. |
| `memos` | Project-wide invariants and conventions. | Used to avoid redundant queries (e.g., build commands). |
| `state` | Current technical anchors (branch, files, ports). | Tracks progress through a multi-step workflow. |
| `history` | Conversation metadata. | Used by the system to manage turn transitions. |

### The `tools` Table
All system tools are available under the `tools` namespace. For example:
- `tools.read_file({path = "...", start_line = 1, end_line = 10, line_numbers = true})`: Reads a file with optional line range and `line_numbers`.
- `tools.grep({pattern = "..."})`: **Preferred** for cross-file searching.
- `tools.file(path)`: Provides an expressive API for file operations.
- `tools.query_db({sql = "..."})`: Queries the project database.

### Asynchronous Execution
Most tools have an `_async` variant or can be used with `dispatch_async`.
```lua
local job1 = tools.dispatch_async("execute_bash", {command = "bazel test //core:test1"})
local job2 = tools.dispatch_async("execute_bash", {command = "bazel test //core:test2"})

-- Perform other logic while tests run...
local results = tools.wait_all(job1, job2)
```

---

## 4. Persistence Mechanisms

### Scratchpad (`tools.manage_scratchpad`)
The scratchpad is the agent's primary "working memory." It should be used to track progress through a plan.
**Mandatory Pattern:**
```lua
-- 1. READ scratchpad at start
local notes = tools.manage_scratchpad({action = "read", key = "notes"})
local ctx = notes and notes.value or {step = 1}

-- 2. PERFORM work...

-- 3. UPDATE scratchpad at end
tools.manage_scratchpad({
    action = "update",
    key = "notes",
    value = {step = 2, status = "Refactored module A"}
})
```

### Memos (`tools.manage_memo`)
Memos are for long-term project invariants. Once the agent learns how to run tests or where a specific configuration is kept, it should store it in a memo to avoid re-discovering it in future sessions.

---

### Transient Scope
*   **Skill Limitation**: The `hey <skill>` hotword detection does not work for non-default or custom skills within a sub-query, as the sub-query's database only contains default system personas.

**Example: Batch Analysis**
```lua
local files = {"auth.cpp", "session.cpp", "db.cpp"}
local jobs = {}
for _, f in ipairs(files) do
    local code = tools.read_file({path = f})
        prompt = "Explain the error handling pattern in this file",
        context = code
    })
end

for i, job in ipairs(jobs) do
    print("Analysis for " .. files[i] .. ": " .. job:wait())
end
```

---

## 6. Best Practices

1.  **Read Before Writing**: Always read the `scratchpad` at the beginning of a script to maintain continuity.
2.  **Filter Aggressively**: Use Lua's string manipulation or `grep` to filter data before returning it from `run_lua`.
4.  **No Uncommitted State**: In the Mail Model workflow, ensure all logical units of work are committed via `tools.git_commit_patch` before ending the script.
