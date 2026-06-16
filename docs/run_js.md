# run_js JavaScript Control Plane

`run_js` is a top-level agent tool for executing a synchronous QuickJS snippet
inside the std::slop process. It is intended for orchestration glue, not for
long-running application logic: batch local tool calls, loop over small repeated
operations, reshape JSON, and return a compact result to the model.

## Contract

- Tool arguments are `{ "code": "...", "input": ... }`; `input` is optional
  and defaults to `{}`.
- The code is a plain JavaScript body. Use `return <json-serializable value>;`
  when the caller needs a result. Optional JSON `input` is exposed as
  `globalThis.input`; use it for source-code/edit payload strings instead of
  embedding those strings in JavaScript literals.
- `tools.*` helpers are synchronous. Do not use top-level `await`, async
  wrappers, or Promise-based helper calls.
- Helper methods validate obvious argument-shape errors before side effects.
- JS-initiated host calls still go through `ToolExecutor`, including tool
  visibility, permission checks, cancellation behavior, and subquery policy.
- Recursive `run_js` calls are rejected.
- One-off snippets are ephemeral. Reusable JavaScript helpers should be saved
  through `tools.persist_function(args)` and discovered through `tools.help()` or
  `/tools js_help`.

## Discoverability

Call `tools.help()` from inside `run_js` to inspect the current helper and
host-tool catalog:

```js
return tools.help();
```

The returned object includes:

- `helpers`: dedicated JS helper methods such as `read_file`, `edit_tool`,
  and `execute_bash`.
- `tools`: enabled host tools and their schema metadata when `query_db` is
  callable from the current scope.
- `top_level`: tools exposed directly to the model.
- `run_js_callable`: host tools that may be invoked from JS.

For host tools without a dedicated helper, use
`tools.dispatch(name, args)` after checking `tools.help()`.

## Quote-safe input payloads

Source-code and edit payload strings should be passed as `input` data, not
embedded in the JavaScript snippet. This avoids quote/escape bugs with backticks,
`${...}`, raw strings, Markdown fences, regular expressions, and other
language-specific text.

Tool call argument shape:

```json
{
  "code": "tools.edit_tool({ path: input.path, edits: input.edits });\nreturn tools.read_file({ path: input.path, start_line: 1, end_line: 80 });",
  "input": {
    "path": "file.cpp",
    "edits": [
      { "op": "replace", "find": "quote-heavy old block", "text": "quote-heavy new block" }
    ]
  }
}
```

Inside the snippet, keep JavaScript as the control plane and treat edit payloads
as JSON data:

```js
tools.edit_tool({ path: input.path, edits: input.edits });
return tools.read_file({ path: input.path, start_line: input.start_line, end_line: input.end_line });
```

## Persisted functions

Use `tools.persist_function(args)` for JavaScript helpers that should be reusable
across `run_js` invocations. The helper validates the local argument shape before
calling the host tool. Persisted functions are listed in
`tools.help().persisted_globals` and in `/tools js_help`.

Required fields:

- `name`: function name.
- `code`: JavaScript source for the helper.

Optional fields:

- `description`: human-readable summary.
- `test_args`: array of dry-run arguments for validation.


## Examples

Minimal read:

```js
return tools.read_file({ path: 'README.md', start_line: 1, end_line: 20 });
```

Batch independent inspections and return a compact object:

```js
function summarize(value, maxLen = 1200) {
  const text = typeof value === 'string' ? value : JSON.stringify(value);
  return text.length > maxLen ? text.slice(0, maxLen) + '\n... [truncated]' : text;
}

const files = tools.list_directory({ path: '.', depth: 1, include_ignored: false });
const matches = tools.grep({
  path: '.',
  pattern: 'run_js',
  fixed_strings: true,
  context: 1,
  limit: 20,
  include_ignored: false
});
return { files: summarize(files), matches: summarize(matches) };
```

Edit with payloads supplied through `input`, then validate in one bounded
snippet. Keep shell commands literal or build them from validated allowlisted
values:

```js
tools.edit_tool({ path: input.path, edits: input.edits });
const changed = tools.read_file({
  path: input.path,
  start_line: input.start_line,
  end_line: input.end_line,
  line_numbers: true
});
const test = tools.execute_bash({
  cwd: '.',
  command: 'bazel test //path:target',
  allow_nonzero_exit: false,
  timeout_seconds: 600
});
return { changed, test };
```

## Operational guidance

- Prefer `run_js` when it reduces multiple local tool calls into one bounded,
  deterministic operation.
- Keep returned data small; summarize or slice large outputs.
- Validate object shapes before loops or side effects.
- Prefer exact `edit_tool` replacements over broad shell rewrites.
- File helper paths are repo-relative; absolute paths and path traversal are
  rejected.
- Keep shell commands literal or construct them only from validated allowlisted
  values. Set explicit shell timeouts; use `allow_nonzero_exit: true` only when
  collecting an expected failure for diagnosis.
