# Persistent Functions Plan

## Phase 1: Database Mechanics & Schema Initialization
**Goal:** Guarantee the storage layer exists and is ready when the system boots.
- [ ] **Task 1.1: Locate `Init`:** Find the `Init` function within `core/database.cpp`.
- [ ] **Task 1.2: Inject Schema Creation:** Add the SQL execution for `CREATE TABLE IF NOT EXISTS js_functions (name TEXT PRIMARY KEY, code TEXT, created_at DATETIME DEFAULT CURRENT_TIMESTAMP);`.
- [ ] **Task 1.3: Error Handling:** Ensure that if this table creation fails, the database initialization logs the error and gracefully handles the failure so the host remains stable.

## Phase 2: Safe-Loading & Cruft Cleanup (The Read-Path)
**Goal:** Implement the "Load, Verify, or Delete" injection protocol in C++ when preparing the JCP.
- [ ] **Task 2.1: Locate JCP Preparation:** Identify the C++ logic where `run_js` prepares the JSContext before executing the agent's script.
- [ ] **Task 2.2: Fetch Functions:** Execute a C++ SQL query to `SELECT name, code FROM js_functions`.
- [ ] **Task 2.3: Compile & Execute Loop:** Iterate over the result set in C++. For each row, push the code to the JSContext and attempt to compile and evaluate it using `JS_Eval` (or equivalent `load`/`pcall` C API sequence) to retrieve the function closure.
- [ ] **Task 2.4: Global Binding:** If execution succeeds and yields a function, bind it to the global environment under the specified `name`.
- [ ] **Task 2.5: Active Cruft Cleanup:** If compilation or execution fails, immediately execute a `DELETE FROM js_functions WHERE name = ?` query from C++ to remove the corrupt function from the database. 

## Phase 3: The `persist_function` API (The Write-Path)
**Goal:** Build a robust, low-boilerplate tool for the LLM to validate and permanently save JavaScript code.
- [ ] **Task 3.1: Tool Registration:** Define `tools.persist_function` within the JavaScript Control Plane environment.
- [ ] **Task 3.2: Argument Parsing:** Accept the table `{ name = string, code = string, test_args = table, expected_result = any }`.
- [ ] **Task 3.3: Syntax Validation:** Run `local func, err = load(code)`. If it fails, return `false, "Syntax Error: " .. err`.
- [ ] **Task 3.4: Runtime Test:** Execute `local success, actual_result = pcall(func, table.unpack(test_args))`. Catch runtime exceptions and return `false, "Runtime Error: " .. actual_result` on failure.
- [ ] **Task 3.5: Simple Verification:** Perform a strict primitive equality check (`actual_result == expected_result`). If they do not match, return `false, "Test Failed: Expected X, got Y"`.
- [ ] **Task 3.6: DB Storage:** If all checks pass, execute `INSERT OR REPLACE INTO js_functions (name, code) VALUES (?, ?)`. Return `true, "Function persisted successfully"`.

## Phase 4: Documentation & Prompt Engineering
**Goal:** Strongly encourage the LLM to utilize this feature for token optimization and context management.
- [ ] **Task 4.1: Update `help` Output:** Modify the dynamic string returned by `tools.help()` to clearly document `tools.persist_function` and its arguments, explicitly noting that `expected_result` handles primitive types.
- [ ] **Task 4.2: Emphasize Usage:** Modify `system_prompt.md` to strongly instruct the LLM to aggressively offload reusable logic into `tools.persist_function`. Frame this as the primary method to save tokens and keep the script payloads lightweight.
- [ ] **Task 4.3: Concrete Example:** Add a highly visible, copy-pasteable example in `system_prompt.md` demonstrating exactly how to draft, test, and persist a function in a single JCP turn.

