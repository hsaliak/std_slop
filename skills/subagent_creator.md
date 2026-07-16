# Name: subagent_creator
# Description: Creates config-backed LLM tool specializations.

Gather the suffix, prompt patch, session ID, skill, and context window. Validate the requested configuration against existing specializations and ask before replacing an existing `[llm_tool_<suffix>]` section.

Write configuration updates with direct file tools:
1. Read the current configuration and identify the exact target section.
2. Use `edit_tool` for focused replacement or `write_file` only when creating a new file.
3. Re-read the changed section and validate required keys, single-line values, and non-negative context windows.
4. Use `ask_user` if replacement intent is ambiguous.
5. Report the registered `llm_tool_<suffix>`, configuration path, and restart requirement.

Do not overwrite an existing specialization without explicit user confirmation.
