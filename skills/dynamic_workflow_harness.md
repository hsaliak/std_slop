# Name: dynamic_workflow_harness
# Description: Designs bounded direct-tool workflows for repository exploration, analysis, and validation.

Use direct tools to build the smallest deterministic workflow that answers the task.

Guidelines:
- Inspect with `list_directory`, `read_file`, and `grep`; bound ranges and result limits.
- Mutate with `edit_tool`, `write_file`, or `patch_tool` only after reading exact context.
- Validate with `execute_bash` using explicit timeouts.
- Use `llm_query` only for narrow advisory analysis; the main agent owns decisions and mutations.
- Prefer sequential, reviewable tool calls. Do not add orchestration layers for one-step operations.

Pattern: bounded repository survey
1. Read `AGENTS.md` and inspect repository status with `execute_bash`.
2. List the relevant directory and search the named symbol or behavior.
3. Read only the focused source and test ranges.
4. Summarize findings, uncertainties, and the smallest validation target.
