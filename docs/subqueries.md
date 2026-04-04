
# Subquery Specializations (INI-based)

This document defines the implementation plan for INI-configured `llm_query`
specializations ("sub-agents") with a strict, non-recursive execution policy.

## Goals

1. Allow users to define specialized LLM tools via config sections.
2. Keep configuration surface intentionally small.
3. Enforce hard safety policy for subqueries:
   - no nested sub-agents
   - no recursive `llm_query`
   - no specialization inheritance from subquery context
4. Keep behavior deterministic and testable.

## Non-goals (for this phase)

- No gRPC/service architecture.
- No full DAG compiler.
- No per-specialization tool allowlist/denylist config.
- No skill inheritance toggles.

---

## Minimal INI schema

Each section with prefix `[llm_tool_<tool_name>]` defines one specialization.

Required keys:

- `system_prompt_patch`
- `session_id`
- `skill`

Optional keys:

- `context_window` (non-negative integer; `0` means infinite history)

Example:

```ini
[llm_tool_code_review_llm]
system_prompt_patch = You are a strict code reviewer focused on correctness and maintainability.
session_id = code_review
skill = code_reviewer
context_window = 8

[llm_tool_explorer_llm]
system_prompt_patch = You explore repository structure and summarize findings with file paths.
session_id = data_explorer
skill = data_explorer
context_window = 20
```

Semantics:

- Presence of section => specialization is registered.
- Absence => specialization is not registered.
- Base behavior is always `llm_query`.

---

## Fixed subquery policy (hardcoded)

For all config-defined subquery tools:

1. Execution scope is `SUBQUERY`.
2. Maximum depth is fixed at `1` (root -> subquery only).
3. Subqueries cannot call:
   - `llm_query`
   - any tool registered from `llm_tool_*`
4. `ask_user` remains disabled in subquery context.
5. Subqueries do not inherit parent specialization config.

Future extension points may relax these constraints, but not in this phase.

---

## Implementation plan

## Phase 1: Config parsing + validation

### Objective
Read `[llm_tool_*]` sections from INI and validate them.

### Changes
- `core/config.h`
  - Add specialization config struct.
  - Add loader API (e.g., `LoadLlmToolSpecializations`).
- `core/config.cpp`
  - Reuse `ParseIni` output to scan section prefix `llm_tool_`.
  - Parse required keys + optional `context_window`.
  - Validation:
    - non-empty specialization name
    - required keys present/non-empty
    - `context_window >= 0` if present (`0` means infinite history)
    - no duplicate names
    - no conflicts with built-in tool names (`llm_query`, etc.)

### Unit tests
- valid config with multiple specializations loads successfully
- missing `session_id`/`skill`/`system_prompt_patch` rejected
- invalid `context_window` rejected
- reserved-name collision rejected

## Phase 2: Registration in startup path

### Objective
Register specializations as tools before interaction loop starts.

### Changes
- `main.cpp`
  - After existing `LoadConfigAndApply(...)`, load specialization config.
  - Register each specialization with `ToolExecutor::RegisterTool`.
  - Handler maps user input to generalized `InteractionEngine::Query` options:
    - fixed `session_id`
    - fixed `skill`
    - optional `context_window`
    - scope/depth marked as subquery

### Unit tests
- registered tool list includes config-defined specializations
- absent config => no specialization registration

## Phase 3: Query options generalization

### Objective
Allow specialization handlers to pass structured query options.

### Changes
- `interface/interaction_engine.h/.cpp`
  - Add `QueryOptions` struct with only:
    - `session_id`
    - `skill`
    - optional `context_window`
    - execution scope/depth metadata
  - Preserve existing default behavior for plain `llm_query`.

### Unit tests
- existing `llm_query` behavior regression coverage
- specialization applies configured session + skill + context window

## Phase 4: Centralized recursion guard

### Objective
Enforce non-recursive subquery policy in one place.

### Changes
- `tools/tool_executor.*` and/or dispatcher boundary
  - Add execution-context-aware guard:
    - if scope is subquery, block `llm_query` and all specialization tools
    - reject depth > 1
  - return deterministic `InvalidArgument` error message

### Unit tests
- subquery -> `llm_query` call rejected
- subquery -> specialization call rejected
- root -> specialization call allowed

---

## Fuzz test plan (new)

Per project guidance, changes touching untrusted structured input should get fuzz
coverage. Add the following fuzz tests.

## 1) Config specialization parser fuzz target

**File (proposed):** `core/config_llm_specializations_fuzz_test.cpp`

**Target behavior:**
- Input: arbitrary bytes treated as INI content.
- Parse with `ParseIni` + specialization loader.
- Must never crash/hang.
- Must either:
  - return valid parsed specializations, or
  - cleanly return error status.

**Assertions:**
- no crash
- no UB sanitizer failure
- duplicate and malformed sections are rejected deterministically

**Seeds:**
- `docs/example_config.ini`
- `docs/example_subqueries.ini`
- minimal/invalid snippets from unit tests

## 2) Subquery policy boundary fuzz target

**File (proposed):** `tools/subquery_policy_fuzz_test.cpp`

**Target behavior:**
- Input: random tool names + scope/depth flags + synthetic call arguments.
- Exercise the centralized policy check before dispatch.
- Ensure forbidden combos are rejected cleanly and cannot reach side effects.

**Assertions:**
- no crash
- forbidden tools in subquery scope always rejected
- depth > 1 always rejected

**Seeds:**
- known tool names from DB/tool registration tests
- specialization-like names (`llm_tool_foo`, `code_review_llm`)

## 3) Query options fuzz target

**File (proposed):** `interface/query_options_fuzz_test.cpp` (or in existing test package)

**Target behavior:**
- Input: random serialized `QueryOptions` fields.
- Validate option normalization (context window bounds, session/skill emptiness).
- Confirm malformed options do not reach execution.

**Assertions:**
- no crash
- invalid options return error
- valid options produce normalized internal form

## BUILD updates

- Add new fuzz tests in package-local `BUILD.bazel` files:
  - `core/BUILD.bazel` for config fuzz target
  - `tools/BUILD.bazel` for policy fuzz target
  - package owning query options for query fuzz target

---

## `system_prompt.md` update plan

Because each user config may define different specialization tool names, the
system prompt should guide behavior generically.

## Required prompt updates

1. Add a section under tool-usage guidance:
   - "If config-defined LLM specialization tools are available, use them for
     bounded delegated tasks (review, exploration, summarization) when helpful."

2. Add recursion rule:
   - "Never attempt to invoke sub-agent tools from within delegated subquery
     execution contexts."

3. Add selection rule:
   - "Prefer specialized tools when the task matches their role; otherwise use
     direct tools."

4. Keep deterministic behavior:
   - "Do not assume specialization names exist; discover available tools first
     and adapt."

## Suggested insertion text

```md
## LLM Specialization Tools (if present)
- Some environments define additional LLM tools via config (for example,
  `code_review_llm`, `explorer_llm`).
- Treat these as bounded delegation tools for focused analysis or review.
- First discover available tools and only use specializations that are present.
- Prefer specializations when task-role fit is strong; otherwise use normal tools.
- Do not attempt recursive delegation through specialization tools.
```

---

## Docs updates

1. Add `docs/example_subqueries.ini` with minimal working example.
2. Link this file from `docs/USERGUIDE.md` and/or `docs/config_impl.md`.
3. Document fixed policy invariants explicitly (no recursion, depth=1).

---

## Acceptance criteria

1. INI-defined `[llm_tool_*]` sections register specialized tools.
2. Required fields validated; invalid config fails clearly.
3. Subquery recursion is blocked centrally and test-covered.
4. Fuzz targets exist for parser, policy boundary, and query options.
5. `system_prompt.md` includes generic specialization guidance.
6. `docs/example_subqueries.ini` exists and is referenced in docs.