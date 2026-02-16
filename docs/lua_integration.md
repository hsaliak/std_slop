# Lua Integration & Control Plane

`std::slop` uses Lua as a high-level orchestration layer and a "control plane" for multi-step task execution. This bridge allows the agent to write and execute scripts that combine multiple tools, handle logic, and perform parallel operations safely.

## 1. Overview

The **lua-integration** branch represents an architectural shift from direct tool execution to a Lua-based orchestration model, implementing the **Recursive Language Model (RLM)** paradigm. This document details the differences between the `lua-integration` branch and the `main` branch, and provides guidelines for effective script orchestration.

## 2. Recursive Language Model (RLM) Paradigm

In the RLM paradigm, the agent processes arbitrarily long contexts by treating the codebase, history, and scratchpad as external variables in a persistent Lua environment. The Lua Control Plane (LCP) serves as the primary entry point for all reasoning and execution.

### 2.1 Symbolic Handles

The LCP provides several global symbolic handles that allow scripts to access and persist state across turns without inflating the primary context window.

| Global | Contents | Read When | Write When |
|--------|----------|-----------|------------|
| `scratchpad` | Persistent working notes & roadmap | Every script start | After every atomic step |
| `memos` | Project invariants & learned conventions | Before expensive queries | After first learning a convention |
| `state` | Current context (branch, file, goal) | Turn start | After git ops, file switches |
| `history` | Conversation metadata (lengths, previews) | Turn start | System-managed |

### 2.2 Mandatory Turn Pattern

To ensure state continuity, scripts must follow a specific "Read-Execute-Write" pattern:

1.  **READ** the scratchpad first to orient the task.
2.  **EXECUTE** the work (investigation, code changes, tool calls).
3.  **WRITE** the scratchpad last to persist progress for the next turn.

```lua
-- 1. READ scratchpad first
local notes = tools.manage_scratchpad({action = "read"})
local ctx = notes and notes.content or "No notes found."

-- 2. DO work...
-- (Example: Investigation or tool execution)

-- 3. WRITE scratchpad last
tools.manage_scratchpad({
    action = "update",
    content = "Completed investigation; next step: apply patches."
})
```

## 2. Git History Comparison

### Branch Statistics
| Metric | main | lua-integration |
|--------|------|-----------------|
| Total commits | 493 | 508 |
| Unique to branch | - | 14 commits |

### Unique Commits in lua-integration

| Commit | Description |
|--------|-------------|
| `51d2099` | Update prompt and test |
| `e8fae00` | orchestrator: Stop injecting scratchpad in prompt |
| `c303b1d` | core: refactor ToolExecutor and add scenario test |
| `b121473` | core: simplify top-level tool registration |
| `dbd9394` | updates |
| `a6f31cb` | remove useless |
| `6b361ae` | docs: Add staging_branches table and sticky logic |
| `68f69a9` | Implement sticky parent branch logic for Mail Model |
| `e233225` | test: add regression test for large number of search tags |
| `34f69d4` | refactor: use CTE and JOIN for memo tag searches |
| `6185f45` | use manage_scratchpad |
| `4a5ed3f` | Remove optimizations.md |
| `b1ec02d` | Fix formatting and content of docs/lua_integration.md |
| `b22a4b4` | Fix slop_guard recursion in git_branch_staging |
| `49c62a1` | Allow HEAD state in slop_guard for verification |

### Merge Base
The branches diverged from a common ancestor at commit `Merge` (commit hash starting with `4e7c0c`).

## 3. Architectural Differences

### 3.1 Tool Execution Model

**main branch:**
- Direct tool registration and execution at the top level
- Tools are called directly from the orchestration layer

**lua-integration branch:**
- Most high-level tools are **disabled by default** in the top-level manifest
- `run_lua` is the primary interface for almost all operations
- Tools must be called from within a Lua script via the `tools` table

### Default Enabled Tools (lua-integration)
- `run_lua`: The orchestration engine
- `query_db`: For schema and metadata discovery
- `llm_query`: For isolated sub-task processing

### 3.2 The `run_lua` Tool

The `run_lua` tool executes a Lua 5.4 script in a sandboxed environment with access to the project's toolset.

#### Environment Globals

| Variable | Type | Description |
|----------|------|-------------|
| `tools` | table | Contains all standard tools. Each tool takes a single table argument |
| `history` | array | Message objects (`{role, content}`) representing the current session history |
| `state` | string | Current persistent technical state |
| `scratchpad` | string | Current persistent plan/notes |
| `llm_query(prompt)` | function | Synchronous helper for isolated LLM sub-tasks |
| `print(...)` | function | Standard Lua print, redirected to tool result |

#### Discovery: `tools.help()`
To see the full manifest of available tools, their signatures, and documentation, call `print(tools.help())` from within a Lua script.

### 3.3 Asynchronous Execution & Parallelism

The Lua bridge supports non-blocking operations for performance-critical tasks.

#### Async Variants
Tools that support asynchronous execution have an `_async` suffix:
- `tools.execute_bash_async({command = "..."})`
- `tools.llm_query_async(query)`
- `tools.read_file_async({path = "..."})`

#### The `Job` Object
Async tools return a `Job` object. Use `job:wait()` to block and retrieve the result.

```lua
local j1 = tools.execute_bash_async({command = "bazel test //core/..."})
local j2 = tools.execute_bash_async({command = "bazel test //interface/..."})

local res1 = j1:wait()
local res2 = j2:wait()
```

### 3.4 Safety: The `slop_guard`

**main branch:**
- Basic protection against main branch modifications

**lua-integration branch:**
- Enhanced `slop_guard` in `preamble_lib.lua` that:
  - Allows operations outside a git repo (e.g., unit tests)
  - Permits `HEAD` state for verification purposes
  - Prevents destructive operations on non-staging branches

```lua
-- lua-integration branch logic
if not branch:find("^slop/staging/") and branch ~= "HEAD" then
  error("Destructive operations are only allowed on 'slop/staging/*' branches...")
end
```

## 4. Core File Differences

### 4.1 core/orchestrator.cpp

**Key Change:** Removed scratchpad injection from `BuildSystemInstructions`

The lua-integration branch removes the automatic injection of scratchpad content into the system prompt:

```diff
-  auto scratchpad_or = db_->GetScratchpad(session_id);
-  if (scratchpad_or.ok() && !scratchpad_or->empty()) {
-    absl::StrAppend(&system_instruction, "## Active Scratchpad\n", *scratchpad_or, "\n");
-  }
```

### 4.2 core/tool_executor.cpp & .h

- Added new `dispatch_map_` for tool routing
- Added `DescribeDatabase()` method for schema discovery
- Updated tool registration pattern for Lua integration

### 4.3 core/preamble_lib.lua

**New Functions:**
- `git.resolve_base_branch(provided_base)`: Resolves the base branch for patch operations
- Enhanced `git_branch_staging` with parent branch tracking
- Added `staging_branches` table management in `git_finalize_series`

**Updated Functions:**
- `git_finalize_series`: Now uses `resolve_base_branch` and cleans up staging branch metadata

### 4.4 Database Schema

**New Table: `staging_branches`**

```sql
CREATE TABLE IF NOT EXISTS staging_branches (
    branch_name TEXT PRIMARY KEY,
    base_branch TEXT NOT NULL
);
```

This enables "sticky" parent branch logic, tracking which base branch each staging branch was created from.

## 5. Documentation Changes

### 5.1 docs/lua_integration.md
- Comprehensive documentation of the Lua control plane
- Added sections on async execution, parallelism, and safety
- Documented new tool patterns and idiomatic usage

### 5.2 docs/SCHEMA.md
- Added `staging_branches` table documentation
- Updated tool manifest to reflect new default-enabled tools

## 6. Feature Summary

| Feature | main | lua-integration |
|---------|------|-----------------|
| Direct tool execution | ✅ | ❌ (via Lua) |
| Lua orchestration | Partial | Full |
| Async tool variants | No | Yes (`_async` suffix) |
| Job-based parallelism | No | Yes |
| slop_guard for safety | Basic | Enhanced (+HEAD) |
| Sticky branch tracking | No | Yes (`staging_branches`) |
| Scratchpad in prompt | Yes | No |
| Database schema discovery | No | Yes (`describe_db`) |

## 7. Idiomatic Usage

### 7.1 Script Structure

Scripts should return a summary of their actions or a specific data structure:

```lua
-- Idiomatic pattern
local files = tools.list_directory({path = "src", depth = 2})
local count = 0
for _, file in ipairs(files) do
  if file:match("%.cpp$") then count = count + 1 end
end
return { cpp_file_count = count }
```

### 7.2 Parallelization and Sub-LLM Queries

The LCP allows for offloading semantic sub-tasks to external LLM calls. This is particularly useful for analyzing large contexts or performing repetitive evaluations.

*   `tools.llm_query`: Synchronous variant for investigative work.
*   `tools.llm_query_async`: Asynchronous variant for large-scale parallel processing.

**Pattern: Batch Read and Parallel Analysis**

```lua
-- BATCH READ: read multiple files concurrently
local files = {"file1.cpp", "file2.cpp", "file3.cpp"}
local jobs = {}

for _, path in ipairs(files) do
    local content = tools.read_file({path = path})
    jobs[#jobs + 1] = tools.llm_query_async({
        prompt = "Analyze this code for bugs",
        context = content
    })
end

-- AWAIT results
local results = {}
for i, job in ipairs(jobs) do
    results[files[i]] = job:wait()
end
return results
```

### 7.3 Chaining Tasks

Use the result of one LLM call as context for the next:

```lua
local summary_job = tools.llm_query_async({
    prompt = "Summarize the changes in this module",
    context = large_diff
})
local summary = summary_job:wait()

local refined_job = tools.llm_query_async({
    prompt = "Format this summary for a CHANGELOG",
    context = summary
})
return refined_job:wait()
```

## 8. Migration Notes

When working with the lua-integration branch:

1. All tool calls must be wrapped in `run_lua` scripts
2. Use `tools.git_branch_staging()` before making changes
3. Use `job:wait()` for async operations
4. Check `tools.help()` for available tools and signatures
5. The `staging_branches` table maintains parent branch context automatically

## 9. Proposed Tweets

Here are 3-4 tweet ideas to announce this feature:

> **Tweet 1 (Announcement):**
> 🚀 Excited to announce Lua integration in std::slop! Now you can write orchestration scripts that combine tools, handle logic, and run parallel operations — all in Lua 5.4. The agent just got a major upgrade. #AI #LLM #Coding

> **Tweet 2 (Technical Deep-dive):**
> 🧵 How std::slop's Lua control plane works:
> • `run_lua` is the new primary interface
> • Tools called via `tools.table` from within scripts
> • Async variants with `job:wait()` for parallelism
> • Safe & sandboxed execution
> Your agent can now write its own code to solve complex tasks.

> **Tweet 3 (Developer Experience):**
> The Lua bridge in std::slop gives agents:
> ✅ Scriptable orchestration
> ✅ Parallel tool execution  
> ✅ Built-in safety guards (`slop_guard`)
> ✅ Schema discovery via `query_db`
> 
> It's like giving your agent its own shell — but safer. #DevTools

> **Tweet 4 (Feature Highlight):**
> New in std::slop: "sticky" parent branch tracking! 
> When you create a staging branch (`slop/staging/...`), it remembers which base branch it came from.
> 
> Perfect for the Mail Model workflow where review cycles need context. 🎯

## 10. Database Discovery (`describe_db`)

The `describe_db` tool allows the agent to inspect the schema of its own ledger. This is crucial for:
- Writing accurate SQL queries for context retrieval.
- Understanding the structure of memos and sessions.
- Discovering internal tables like `staging_branches`.

Usage in Lua:
```lua
local schema = tools.describe_db({})
print(schema)
```

## 11. Automated Mail Model

With the introduction of the `staging_branches` table, the `lua-integration` branch significantly automates the Mail Model workflow. When `git_branch_staging` is called, it records the parent branch. Tools like `git_finalize_series` can then automatically resolve the correct base branch for merging or submitting patches, reducing manual configuration errors.
