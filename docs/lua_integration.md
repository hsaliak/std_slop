# Lua Integration & Control Plane

`std::slop` uses Lua as a high-level orchestration layer and a "control plane" for multi-step task execution. This bridge allows the agent to write and execute scripts that combine multiple tools, handle logic, and perform parallel operations safely.

## 1. Orchestration Philosophy

As of recent architectural updates, `run_lua` is the primary interface for almost all operations. To ensure strict orchestration and reproducibility, **most high-level tools are disabled by default** in the top-level manifest.

### Default Enabled Tools
- `run_lua`: The orchestration engine.
- `query_db`: For schema and metadata discovery.
- `llm_query`: For isolated sub-task processing.

All other tools (e.g., `read_file`, `write_file`, `execute_bash`, `git_*`) must be called from within a Lua script via the `tools` table.

## 2. The `run_lua` Tool

The `run_lua` tool executes a Lua 5.4 script in a sandboxed environment with access to the project's toolset.

### Environment Globals
- **`tools`**: A table containing all standard tools. Every tool takes a **single table argument** (e.g., `tools.read_file({path='foo.txt'})`).
- **`history`**: An array of message objects (`{role, content}`) representing the current session history. This allows scripts to programmatically extract information from previous turns.
- **`state`**: A string containing the current persistent technical state.
- **`scratchpad`**: A string containing the current persistent plan/notes.
- **`llm_query(prompt)`**: A synchronous helper to perform isolated LLM sub-tasks.
- **`print(...)`**: Standard Lua print, redirected to the tool result for debugging and logging.

### Discovery: `tools.help()`
To see the full manifest of available tools, their signatures, and documentation, call `print(tools.help())` from within a Lua script.

## 3. Asynchronous Execution & Parallelism

The Lua bridge supports non-blocking operations for performance-critical tasks (e.g., running multiple tests or querying the LLM in parallel).

### Async Variants
Tools that support asynchronous execution have an `_async` suffix:
- `tools.execute_bash_async({command = "..."})`
- `tools.llm_query_async(query)`
- `tools.read_file_async({path = "..."})`

### The `Job` Object
Async tools return a `Job` object. Use `job:wait()` to block and retrieve the result.

**Example:**
```lua
local j1 = tools.execute_bash_async({command = "bazel test //core/..."})
local j2 = tools.execute_bash_async({command = "bazel test //interface/..."})

local res1 = j1:wait()
local res2 = j2:wait()

print("Core tests: " .. res1.stdout)
print("Interface tests: " .. res2.stdout)
```

## 4. Safety & The `slop_guard`

To prevent accidental modification of the `main` branch, destructive tools (like `write_file`, `apply_patch`, or `execute_bash`) are protected by a `slop_guard`.

- **Enforcement**: These tools will throw an error if called while the current Git branch is not a staging branch (prefixed with `slop/staging/`).
- **Workflow**: Agents must first use `tools.git_branch_staging({name = "..."})` to create a safe sandbox before performing modifications.

## 5. Idiomatic Usage

Scripts should typically return a summary of their actions or a specific data structure. The final expression or explicit `return` in the script is captured as the tool result.

```lua
-- Idiomatic pattern
local files = tools.list_directory({path = "src", depth = 2})
local count = 0
for _, file in ipairs(files) do
  if file:match("%.cpp$") then count = count + 1 end
end
return { cpp_file_count = count }
```
