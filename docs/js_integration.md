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

### Persistence and State Continuity
The JCP provides a persistent environment across turns. By using the `scratchpad` and `state` globals, the agent maintains internal continuity without bloating user-facing output. These globals are saved in sqlite at the end of a turn, and injected into the environment in the next.

---

## 2. The Recursive Language Model (RLM) Paradigm

The JCP implements the **Recursive Language Model (RLM)** paradigm. In this model, the agent processes arbitrarily long contexts by treating the codebase, history, and scratchpad as external variables in a persistent JavaScript environment.

The JCP acts as the "inner loop" of the agent's cognition:
1.  **Analyze**: The LLM analyzes the current state and goal.
2.  **Orchestrate**: The LLM writes a JavaScript script to perform the next logical step.
3.  **Execute**: The JCP executes the script, interacting with the filesystem, database, and sub-LLMs.
4.  **Refine**: The results are returned to the LLM to refine the next step.

---

## 3. The JavaScript Environment

Scripts executed via `run_js` have access to a rich environment tailored for software engineering.

### Global Symbolic Handles
These variables bridge the gap between individual turns and provide persistent context.

| Global | Purpose | Usage |
| :--- | :--- | :--- |
| `scratchpad` | Working notes and checklists. | Read at turn start; update via `tools.manage_scratchpad`. |
| `state` | Current technical anchors (branch, files, ports). | Tracks progress through a multi-step workflow. |
| `history` | Conversation metadata. | Used by the system to manage turn transitions. |

### The `tools` Table
All system tools are available under the `tools` namespace. Examples include:
- `tools.read_file({path = "...", start_line = 1, end_line = 10, line_numbers = true})`: Reads a file with optional line range and `line_numbers`.
- `tools.grep({pattern = "..."})`: **Preferred** for cross-file searching.
- `tools.query_db({sql = "..."})`: Queries the project database.
- `tools.ask_user({prompt = "..."})`: Prompts the human user for input. This is synchronous and will block script execution until the user responds.
  - **Behavior**: If the user attempts to use a slash command in response, the system will display an error and re-prompt them. This ensures the agent receives a valid response to its question.
- `tools.help({})`: Returns a JSON manifest with tool contracts, canonical names, and aliases.

### Asynchronous Execution
Most tools have an `_async` variant or can be used with `dispatch_async`.
```javascript
const job1 = tools.dispatch_async("execute_bash", {command: "bazel test //core:test1"});
const job2 = tools.dispatch_async("execute_bash", {command: "bazel test //core:test2"});

// Perform other logic while tests run...
const res1 = job1.wait();
const res2 = job2.wait();
```

---

## 4. Persistence Mechanisms

### Scratchpad (`tools.manage_scratchpad`)
The scratchpad is the agent's primary internal "working memory." It should be used to track progress through a plan, not as user-facing output.
**Mandatory Pattern:**
```javascript
// 1. READ scratchpad at start
const notes = tools.manage_scratchpad({action: "read", key: "notes"});
const ctx = notes ? notes : {step: 1};

// 2. PERFORM work...

// 3. UPDATE scratchpad at end
tools.manage_scratchpad({
    action: "update",
    key: "notes",
    value: {step: 2, status: "Refactored module A"}
});
```

### Transient Scope
*   **Skill Limitation**: The `hey <skill>` hotword detection does not work for non-default or custom skills within a sub-query, as the sub-query's database only contains default system personas.

**Example: Batch Analysis**
```javascript
const files = ["auth.cpp", "session.cpp", "db.cpp"];
const jobs = [];
for (const f of files) {
    const code = tools.read_file({path: f});
    jobs.push(tools.dispatch_async("ask_user", {
        prompt: "Explain the error handling pattern in this file",
        context: code
    }));
}

for (let i = 0; i < jobs.length; i++) {
    print("Analysis for " + files[i] + ": " + jobs[i].wait());
}
```

---

## 6. Best Practices

1.  **Read Before Writing**: Always read the `scratchpad` at the beginning of a script to maintain continuity.
2.  **Filter Aggressively**: Use JavaScript processing or `grep` to filter data before returning it from `run_js`.
3.  **Return User Conclusions**: Keep large intermediate artifacts in scratchpad, but return a concise user-facing conclusion in the same turn.
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
