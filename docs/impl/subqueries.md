# Subquery Specializations

INI sections configure specialized `llm_query` tools. For setup, see [../WALKTHROUGH.md](../WALKTHROUGH.md) and [../example_subqueries.ini](../example_subqueries.ini).

## INI Schema

Each `[llm_tool_<tool_name>]` section defines one specialization.

Required keys:
- `system_prompt_patch`
- `session_id`
- `skill`

Optional keys:
- `context_window` (`0` means unlimited delegated history)

```ini
[llm_tool_code_review_llm]
system_prompt_patch = You are a strict code reviewer focused on correctness and maintainability.
session_id = code_review
skill = code_reviewer
context_window = 8
```

## Policy Boundary

Config-defined subquery tools:

1. Run with `SUBQUERY` scope and maximum depth `1`.
2. Cannot call `llm_query` or other `llm_tool_*` tools.
3. Cannot use `ask_user`.
4. Do not inherit parent specialization configuration.
