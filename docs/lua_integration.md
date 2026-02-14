### FILE: docs/lua_integration.md | TOTAL_LINES: 69 | RANGE: 1-69
1: # Lua Integration & Control Plane
2: 
3: `std::slop` uses Lua as a high-level orchestration layer and a "control plane" for complex task execution. This bridge allows the agent to write and execute scripts that combine multiple tools, handle logic, and perform parallel operations safely.
4: 
5: ## 1. The `lua_control_plane` Persona
6: 
7: When the `lua_control_plane` skill is active, the agent enters a restricted execution mode:
8: - **Direct Tooling Disabled**: The agent is prohibited from calling tools like `read_file`, `write_file`, or `execute_bash` directly from the chat interface.
9: - **Orchestration-Only**: All operations must be performed by writing and executing a Lua script via the `run_lua` tool.
10: - **Reproducibility**: This ensures that complex sequences of operations are captured as a single, reproducible script in the session history.
11: 
12: ## 2. The `run_lua` Tool

As of recent architectural updates, `run_lua` is the primary interface for almost all operations. To ensure strict orchestration and reproducibility, **most high-level tools are disabled by default** in the top-level manifest.

### Default Enabled Tools
- `run_lua`: The orchestration engine.
- `query_db`: For schema and metadata discovery.
- `llm_query`: For isolated sub-task processing.

All other tools (e.g., `read_file`, `write_file`, `execute_bash`, `git_*`) must be called from within a Lua script via the `tools` table.

## 2. The `run_lua` Tool
13: 
14: The `run_lua` tool executes a Lua 5.4 script in a sandboxed environment with access to the project's toolset.
15: 
16: ### Environment Globals
17: - **`tools`**: A table containing all standard tools. Every tool takes a **single table argument** (e.g., `tools.read_file({path='foo.txt'})`).
18: - **`history`**: An array of message objects (`{role, content}`) representing the current session history. This allows scripts to programmatically extract information from previous turns.
19: - **`state`**: A string containing the current persistent technical state.
20: - **`scratchpad`**: A string containing the current persistent plan/notes.
21: - **`llm_query(prompt)`**: A synchronous helper to perform isolated LLM sub-tasks.
22: - **`print(...)`**: Standard Lua print, redirected to the tool result for debugging and logging.
23: 
24: ### Discovery: `tools.help()`
25: To see the full manifest of available tools, their signatures, and documentation, call `print(tools.help())` from within a Lua script.
26: 
27: ## 3. Asynchronous Execution & Parallelism
28: 
29: The Lua bridge supports non-blocking operations for performance-critical tasks (e.g., running multiple tests or querying the LLM in parallel).
30: 
31: ### Async Variants
32: - `tools.execute_bash_async({command = "..."})`
33: - `llm_query_async(prompt)`
34: 
35: ### The `Job` Object
36: Async tools return a `Job` object. Use `job:wait()` to block and retrieve the result.
37: 
38: **Example:**
39: ```lua
40: local j1 = tools.execute_bash_async({command = "bazel test //core/..."})
41: local j2 = tools.execute_bash_async({command = "bazel test //interface/..."})
42: 
43: local res1 = j1:wait()
44: local res2 = j2:wait()
45: 
46: print("Core tests: " .. res1.stdout)
47: print("Interface tests: " .. res2.stdout)
48: ```
49: 
50: ## 4. Safety & The `slop_guard`
51: 
52: To prevent accidental modification of the `main` branch, destructive tools (like `write_file`, `apply_patch`, or `execute_bash`) are protected by a `slop_guard`.
53: 
54: - **Enforcement**: These tools will throw an error if called while the current Git branch is not a staging branch (prefixed with `slop/staging/`).
55: - **Workflow**: Agents must first use `tools.git_branch_staging({name = "..."})` to create a safe sandbox before performing modifications.
56: 
57: ## 5. Idiomatic Usage
58: 
59: Scripts should typically return a summary of their actions or a specific data structure. The final expression or explicit `return` in the script is captured as the tool result.
60: 
61: ```lua
62: -- Idiomatic pattern
63: local files = tools.list_directory({path = "src", depth = 2})
64: local count = 0
65: for _, file in ipairs(files) do
66:   if file:match("%.cpp$") then count = count + 1 end
67: end
68: return { cpp_file_count = count }
69: ```
