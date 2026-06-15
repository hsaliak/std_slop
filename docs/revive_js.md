
# Revive JavaScript Control Plane

This document tracks the mail-mode implementation of the revived JavaScript
Control Plane. Check boxes are updated after each bundle lands with the tests
that verified the bundle.

## Target architecture

Provider-facing orchestration should converge on a single `run_js` tool. The
JavaScript runtime exposes a `tools` object backed by host policy:

```text
Model -> run_js -> QuickJS runtime -> call_tool(name, args) -> ToolExecutor
```

All JS-initiated tool calls must pass through `ToolExecutor` so existing
argument validation, cancellation, tool-call accounting, mail-mode guards, and
subquery policy remain authoritative.

## Bundles

- [x] Bundle 0: Create this tracking document on a staging branch.
  - Validation: `git status --short --branch` confirmed
    `slop/staging/revive-js`.
- [ ] Bundle 1: Reintroduce QuickJS runtime and a `run_js` tool skeleton.
  - Unit coverage: script argument validation, JSON return conversion, syntax
    errors, thrown errors, and non-JSON-serializable returns.
  - Fuzz coverage: malformed `run_js` argument payloads and arbitrary scripts
    must fail cleanly or return JSON without crashing.
- [ ] Bundle 2: Bridge JS `call_tool`/`tools.*` calls through `ToolExecutor`.
  - Unit coverage: known tool call, unknown tool rejection, invalid bridge
    argument rejection, and existing tool validation preservation.
  - Fuzz coverage: arbitrary bridge names and payload shapes are rejected
    cleanly before side effects.
- [ ] Bundle 3: Preserve LLM/subquery policy through JS calls.
  - Unit/integration coverage: normal-scope `llm_query`/`llm_tool_*` routing and
    subquery-scope rejection remain enforced by `ToolExecutor`.
  - Fuzz coverage: generated LLM-like tool names in subquery scope cannot bypass
    policy through JS.
- [ ] Bundle 4: Restore the JavaScript tool library/bootstrap.
  - Unit coverage: `tools.help()`, representative file/search helpers, library
    load failure reporting, and JSON-serializable helper returns.
  - Fuzz coverage: helper entry points validate untrusted shapes before calling
    side-effecting host tools.
- [ ] Bundle 5: Update prompt guidance and verify the full patch series.
  - Validation: affected Bazel targets, fuzz smoke tests, and
    `git_verify_series` over the chosen deterministic command.

## Progress log

- Bundle 0 complete: staging branch created and this document added.