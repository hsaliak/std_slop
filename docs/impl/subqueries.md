# Subquery Specializations (INI-based)

> Implementation/design-history note: this document is not the primary user guide. For current setup instructions, start with [../WALKTHROUGH.md](../WALKTHROUGH.md).

This document captures the implementation plan and policy notes for INI-configured `llm_query` specializations (sub-agents).

## Goals

1. Allow users to define specialized LLM tools via config sections.
2. Keep configuration surface intentionally small.
3. Enforce hard safety policy for subqueries:
   - no nested sub-agents
   - no recursive `llm_query`
   - no specialization inheritance from subquery context
4. Keep behavior deterministic and testable.

## Minimal INI Schema

Each section with prefix `[llm_tool_<tool_name>]` defines one specialization.

Required keys:
- `system_prompt_patch`
- `session_id`
- `skill`

Optional keys:
- `context_window` (`0` means infinite history)

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

## Fixed Policy Boundary

For all config-defined subquery tools:

1. Execution scope is `SUBQUERY`.
2. Maximum depth is fixed at `1`.
3. Subqueries cannot call:
   - `llm_query`
   - any tool registered from `llm_tool_*`
4. `ask_user` remains disabled in subquery context.
5. Subqueries do not inherit parent specialization config.

## Implementation Areas

- Parse `[llm_tool_*]` sections from INI.
- Validate required keys and reserved-name collisions.
- Register specializations during startup.
- Enforce policy boundaries in dispatch/execution.
- Add unit tests and fuzz coverage for malformed config and boundary enforcement.

## Related Files

- [../example_subqueries.ini](../example_subqueries.ini)
- [../WALKTHROUGH.md](../WALKTHROUGH.md)
- [../README.md](../README.md)
