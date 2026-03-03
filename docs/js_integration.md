# JavaScript Control Plane & Orchestration

`std::slop` uses (QuickJS (ES2020+)) QuickJS (ES2020+) as its high-level orchestration layer and "control plane." Instead of executing single tools in isolation, the agent writes and executes JavaScript scripts that combine tools, handle complex logic, and perform parallel operations safely.

## 1. The Core Philosophy: Why JavaScript?

The primary reason for the JavaScript Control Plane (JCP) is to solve the **Context Rot** problem inherent in large-scale LLM interactions.

### Code as a Scalpel
Traditional agents often ingest raw, massive datasets into their context window (e.g., reading a 2000-line file just to find one function). This leads to "context rot" where the model's reasoning is degraded by irrelevant information.
In `std::slop`, the JCP allows the agent to  pass the context programatically into a sub-query and evaluate the provided results, avoiding rot.
- Instead of reading a whole file, a JavaScript script can grep for a pattern, process the result in-memory, and only return the relevant snippets.
- Data filtering happens *within* the JCP, not the LLM's context window.

### Parallelism and Efficiency
The JCP supports asynchronous execution. An agent can initiate multiple file reads, code searches, or even sub-LLM queries simultaneously using `_async` tool variants, drastically reducing the latency of complex investigative tasks.

---

## 2. The Recursive Language Model (RLM) Paradigm

The JCP acts as the "inner loop" of the agent's cognition:
1.  **Analyze**: The LLM analyzes the current state and goal.
2.  **Orchestrate**: The LLM writes a JavaScript script to perform the next logical step.
3.  **Execute**: The JCP executes the script, interacting with the filesystem, database, and sub-LLMs.
4.  **Refine**: The results are returned to the LLM to refine the next step.

---

## 3. The JavaScript Environment

Scripts executed via `run_js` have access to a rich environment tailored for software engineering.

---

## 4. Persistence Mechanisms

### Transient Scope
*   **Skill Limitation**: The `hey <skill>` hotword detection does not work for non-default or custom skills within a sub-query, as the sub-query's database only contains default system personas.

## 5. Offloading via `llm_query`

The JCP allows the agent to "fork" its reasoning by calling sub-LLMs.
- `tools.llm_query`: Synchronous; best for small, investigative tasks.
- `tools.llm_query_async`: Parallel; best for processing large batches of data (e.g., summarizing 10 files at once).

### Transient Scope
Sub-queries spawned via `llm_query` operate within a transient, in-memory database context.
*   **Skill Limitation**: The `hey <skill>` hotword detection does not work for non-default or custom skills within a sub-query, as the sub-query's database only contains default system personas.
*   **Isolation**: Messages and state changes within an `llm_query` do not persist in the main `slop.db` history.

**Example: Batch Analysis**
```javascript
const files = ["auth.cpp", "session.cpp", "db.cpp"];
const jobs = [];
for (const f of files) {
    const code = tools.read_file({path: f});
    jobs.push(tools.llm_query_async({
        query: "Explain the error handling pattern in this file:\n" + code
    }));
}

for (let i = 0; i < jobs.length; i++) {
    print("Analysis for " + files[i] + ": " + jobs[i].wait());
}
```

---

## 6. Best Practices

2.  **Filter Aggressively**: Use JavaScript processing or `grep` to filter data before returning it from `run_js`.
4.  **No Uncommitted State**: In the Mail Model workflow, ensure all logical units of work are committed via `tools.git_commit_patch` before ending the script.

## Persistent Functions

The `tools.persist_function` tool allows you to save JavaScript functions to the local database. These functions are automatically loaded into the global scope (`globalThis`) of every `run_js` execution, making them available as first-class utilities.

### Usage

```javascript
tools.persist_function({
  name: "string",           // The name of the function in the global scope
  code: "string",           // The JS code that returns the function closure
  description: "string",    // (Optional) A description for tools.help()
  test_args: [any],         // (Optional) Arguments to test the function
  expected_result: any      // (Optional) Expected result of the test
});
```

### Example

```javascript
tools.persist_function({
  name: "grep_git_log",
  code: `
    return function(pattern) {
      const logs = tools.execute_bash({command: "git log --oneline"});
      return logs.split("\\n").filter(line => line.includes(pattern));
    }
  `,
  description: "Searches git commit messages for a specific pattern.",
  test_args: ["fix"],
  expected_result: [] // Assuming no 'fix' commits in a fresh repo
});
```

### Discoverability

All persistent functions are automatically listed in the JSON output of `tools.help()` under `persistent_functions`.

## 7. Migration Notes (since v0.1.9)

### Dynamic tool registry (`js_functions`)
JavaScript tools are now loaded from a dynamic registry (`js_functions`) rather than a fixed static list.
In practice, rely on `tools.help()` as the runtime source of truth for tool names, schemas, and availability.

### JSON-default tool result framing
Tool results are framed as JSON by default. This improves consistency for programmatic consumption inside `run_js` scripts.
When writing orchestration scripts, prefer explicit parsing/inspection of returned objects over string-shape assumptions.

### Removed legacy globals
Legacy bridge globals used by older flows (including scratchpad/history globals) are no longer part of the default model contract.
Use explicit tool calls for context retrieval and state handling instead of relying on implicit global session objects.


