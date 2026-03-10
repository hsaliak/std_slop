# TOOL_MIGRATION.md

## Goal
Port the finalized tool surface back into C++ under `tools/` with strict behavioral parity, using a simple **non-templated registry + direct typed parsing in handlers** design.

This plan is execution-oriented and TDD-driven. It assumes we already have substantial tests and should reuse/adapt them first before introducing new test-only scaffolding.

---

## Locked Canonical Scope

### Core tools
1. `describe_db`
2. `git_create_staging_branch`
3. `grep`
4. `grep_tool`
5. `list_directory`
6. `read_file`
7. `use_skill`

### Mail-model tools (slop-guarded)
1. `execute_bash`
2. `git_commit_patch`
3. `git_finalize_series`
4. `git_format_patch_series`
5. `git_reroll_patch`
6. `parse_tool_rows`
7. `patch_tool`
8. `write_file`

### Dropped tools (must not be exposed)
1. `execute_bash_async`
2. `help`
3. `llm_query_async`
4. `shell_escape`

---

## Non-Negotiable Behavioral Anchors

1. Keep `CheckSlopGuard` semantics unchanged:
   - If `settings.mode == "standard"` => bypass guard.
   - Otherwise require branch `slop/staging/*` or `HEAD`.
2. Preserve the exact denial error string:
   - `Destructive operations are only allowed on 'slop/staging/*' branches. Current branch: <branch>`
3. Preserve existing schema validation and output envelope behavior tool-by-tool.
4. Do not reintroduce dropped tools through aliases, registry leftovers, or docs examples.

---

## C++ Implementation Pattern (Simple Struct + Typed Parsing)

Use a **runtime-uniform non-templated registry** and parse typed args directly in handlers with `ParseArgs<T>()`.

### 1) Runtime registry contract (non-templated)

```cpp
using Json = nlohmann::json;
using ToolHandler = std::function<StatusOr<Json>(const Json&, ToolContext&)>;

enum class ToolClass {
  kCore,
  kMailModel,
};

struct ToolSpec {
  std::string name;
  std::string description;
  Json schema;
  ToolClass tool_class;
  ToolHandler handler;
};
```

Why: keeps dispatch/policy/error envelope deterministic and easy to debug.

### 2) Typed parsing pattern (simplified)

Good call from review: keep this simple.

Default approach (recommended):
- Keep `ToolHandler` non-templated.
- Parse typed args directly inside each handler.
- Use `ParseArgs<T>()` only as a helper, not as an additional adapter layer.

```cpp
StatusOr<Json> HandleWriteFile(const Json& raw, ToolContext& ctx) {
  ASSIGN_OR_RETURN(WriteFileArgs args, ParseArgs<WriteFileArgs>(raw));
  // ... tool logic ...
  return Json{{"ok", true}};
}
```


### 3) Where templates are allowed vs avoided

Allowed:
- `ParseArgs<T>()` and field helpers
- reusable arg validation utilities
- optional lightweight adapter helper only if duplication becomes significant

Avoided:
- templated global registry
- templated dispatch/invoke path
- templated error envelope serialization

---

## Policy / Guard Wiring

The executor flow should be:

1. Resolve `ToolSpec` by name.
2. Validate JSON args against schema/contract.
3. If `tool_class == kMailModel`, call `CheckSlopGuard(ctx)`.
4. Execute handler.
5. Normalize response/error envelope exactly as today.

Pseudo-shape:

```cpp
StatusOr<Json> ExecuteTool(const ToolSpec& spec, const Json& args, ToolContext& ctx) {
  RETURN_IF_ERROR(ValidateSchema(spec.schema, args));

  if (spec.tool_class == ToolClass::kMailModel) {
    RETURN_IF_ERROR(CheckSlopGuard(ctx));
  }

  return spec.handler(args, ctx);
}
```

---

## TDD-Driven Migration Strategy (Leverage Existing Tests for Parity)

### Progress Update (ongoing)

- ✅ Top-level promoted JS allowlist now matches locked Core + Mail-model scope.
- ✅ Dropped tools are explicitly covered by top-level NOT_FOUND tests.
- ✅ Added parity tests for slop-guard exact denial string and standard-mode bypass behavior.
- ✅ Normalized top-level slop-guard denial mapping to `FAILED_PRECONDITION` while preserving exact denial text.
- ✅ Added top-level registration parity tests for all Wave 1 (Core) and Wave 2 (Mail-model) promoted tools (asserting non-NOT_FOUND dispatch).
- ✅ Implemented direct C++ top-level handlers for `describe_db`, `use_skill`, and `grep_tool` (adapter delegating to canonical `grep`).
- ✅ Implemented direct C++ top-level handlers for `read_file` and `list_directory`.
- ✅ Implemented direct C++ top-level handlers for `grep` and `git_create_staging_branch`.
- ✅ Updated top-level tests to lock direct-status semantics where handlers now run in C++.
- ✅ Repository-wide validation clean after Wave 1 completion: `bazel test //...` and `bazel build //...` both pass.
- ✅ Wave 2 hardening sweep added explicit slop-guard coverage for all locked mail-model tools with exact denial status/message assertions.
- ✅ Parity gap audit added targeted tests for direct `grep` (`fixed_strings`, truncation marker, ignore list behavior) and direct `git_create_staging_branch` (existing-branch path + DB record expectations).
- ✅ Consolidation pass started: direct C++ handlers added for `write_file` and `parse_tool_rows`; JS fallback status normalization centralized.
- ✅ Continued Wave 2 migration: direct C++ handler added for `execute_bash` with staging-guard reuse and top-level structured return parity.
- ✅ Migrated `patch_tool` to an in-process C++ unified diff applier matching JS semantics (hunk parsing, relaxed matching, dry-run/apply result shapes).
- ✅ Promoted remaining Wave 2 git workflow tools to direct top-level C++ handlers.
- ✅ Removed JS delegation for canonical mail git workflows; logic now implemented in-process C++ with helper-equivalent behavior.
- 🔄 Next: continue wave-based migration checks and tighten central policy/error handling where still path-dependent.

## Guiding principle
Before implementation changes, use existing tests as parity anchors. Extend them minimally to encode current JS behavior and expected C++ behavior.

### Phase A: Snapshot parity expectations

1. Enumerate existing tests for tool execution, policy gate behavior, schema validation, and error formatting.
2. Mark each as:
   - reusable as-is,
   - needs fixture updates,
   - missing (add).
3. Add a canonical `tool_classification_test` that asserts the exact Core/Mail-model/dropped lists.

Success criteria:
- We have a failing/expected test matrix for all tools before porting logic.

### Phase B: Red-Green per tool (wave-based)

For each tool, follow:
1. **Red**: Add/enable parity test case for that tool first.
2. **Green**: Implement tool in C++ registry + handler.
3. **Refactor**: Move shared validation/parsing into typed adapter utilities if duplication appears.

Do not port multiple tools without test checkpoints.

### Phase C: Policy gate tests (mail-model)

For every Mail-model tool, verify:
- Allowed in `settings.mode == "standard"` regardless of branch.
- Denied when mode != standard and branch is not `slop/staging/*` nor `HEAD`.
- Allowed for `slop/staging/*` and `HEAD`.
- Denial text exactly matches anchor string.

### Phase D: Dropped-tool negative tests

Add explicit tests that dropped tools resolve to unknown/not-registered behavior:
- `execute_bash_async`
- `help`
- `llm_query_async`
- `shell_escape`

### Phase E: Regression and conformance suite

Run full existing test suite + new migration tests.
Focus on:
- error envelope compatibility
- schema/parser parity
- tool lookup and dispatch consistency

---

## Tool Port Order (Execution Plan)

## Wave 1: Core tools
1. `read_file` ✅ (top-level C++ handler)
2. `list_directory` ✅ (top-level C++ handler)
3. `describe_db` ✅ (top-level C++ handler)
4. `grep` ✅ (top-level C++ handler)
5. `grep_tool` ✅ (top-level C++ adapter to `grep`)
6. `use_skill` ✅ (top-level C++ handler)
7. `git_create_staging_branch` ✅ (top-level C++ handler)

## Wave 2: Mail-model tools
1. `write_file` ✅ (top-level C++ handler)
2. `patch_tool` ✅ (in-process C++ unified diff applier)
3. `execute_bash` ✅ (top-level C++ handler)
4. `parse_tool_rows` ✅ (top-level C++ handler)
5. `git_commit_patch` ✅ (full in-process C++ implementation)
6. `git_format_patch_series` ✅ (full in-process C++ implementation)
7. `git_reroll_patch` ✅ (full in-process C++ implementation)
8. `git_finalize_series` ✅ (full in-process C++ implementation)

Reasoning: foundational primitives first, then workflow/composed git operations.

---

## Test Matrix Template (per tool)

For each tool, require at least:

1. **Registration test**
   - Tool exists in registry.
   - Classification is correct.
2. **Schema validation test**
   - Valid args accepted.
   - Missing/invalid args rejected with expected error shape.
3. **Behavior parity test**
   - Compare key output/error fields to current expectations.
4. **Policy test**
   - Core: no slop-guard dependency.
   - Mail-model: slop-guarded with branch/mode matrix.

Optional but recommended:
- edge-case tests from historical incidents/bugs.

---

## Repo Hygiene / Drift Prevention

1. Add startup or unit assertions:
   - no duplicate tool names
   - every registry entry has schema + handler
   - dropped tools absent
2. Add a canonical test fixture containing the locked lists.
3. Update docs in tandem with code and tests in same change where feasible.

---

## Commit Plan (Suggested)

1. `tools: add ToolClass + canonical registry contract`
2. `tools: wire mail-model policy gate via CheckSlopGuard`
3. `tools: add typed handler adapter and ParseArgs<T> scaffolding`
4. `tools(core): port read/list/describe/grep/grep_tool`
5. `tools(core): port use_skill + git_create_staging_branch`
6. `tools(mail): port write_file + patch_tool + execute_bash`
7. `tools(mail): port parse_tool_rows + git_commit_patch`
8. `tools(mail): port git_format_patch_series + git_reroll_patch + git_finalize_series`
9. `tools: drop deprecated tool registrations and add negative tests`
10. `docs: update migration and tool classification documentation`

Each commit should keep tests green (or include the red->green transition fully within the commit).

---

## Definition of Done

Migration is complete when all are true:

1. Only locked Core + Mail-model tools are registered.
2. All dropped tools are absent and covered by negative tests.
3. Mail-model tools are slop-guarded with exact semantics and error text.
4. Existing and new parity tests pass.
5. Documentation reflects final canonical classification and extension pattern.

---

## Notes for Future Additions

When adding a new tool later:
1. Classify as Core vs Mail-model first.
2. Add schema + typed args parser.
3. Register through non-templated `ToolSpec` with templated adapter.
4. Add TDD cases (registration, schema, behavior, policy).
5. Update canonical classification docs/tests.















