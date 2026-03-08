## v0.14.1 - 2026-03-08
- Fixed an issue with git_finalize_series where a subsequent execute_bash in the same script would fail as we are in mail mode and destructive ops are not allowed. Now, when we finalize series, we remove mail mode. 
## v0.14 - 2026-03-08

- Mail patch workflow now enforces tool-contract checks, staging-branch use, and approval/hash matching before `git_finalize_series`.
- Patch verification now requires an explicit project test/build command when calling `git_verify_series`.
- `apply_patch` now supports `dry_run` to validate patch anchors/replacements without writing files.
- Updated and fixed `/models` documentation and behavior for provider model listing.

## v0.13 - 2026-03-05

- **Prompt and guidance refreshes**: Reworked `system_prompt.md` with new reliability rules, patcher prompt guidance, and repeated prompt cleaning so the orchestration flow reflects the latest requirements.
- **Tooling standardization**: Updated numerous JS helpers (e.g., `git_grep`, `help`, `git_verify_series`, `persist_function`, `execute_bash`) to return structured objects or throw on error, ensure gitignore compliance, and documented the consistent “Tool Return Values” contract in the USERGUIDE.
- **Testing and interpreter hardening**: Added a real `js-bridge` interpreter gtest, improved JS parser loading/syntax tests, and shared DB row parsing helpers to bolster integration coverage.
- **UX polish**: Applied Gruvbox-inspired semantic colors to tool results to keep the interface more consistent.
- **Responses model tuning**: Introduced the `reasoning` selector for the `Responses` model, updating orchestrator headers, implementation, tests, and the command handler to expose the new option.

## v0.12.2

- **Introduced `git_grep`**: Added the structured/raw repository search tool, locked down its JSON contract with integration and typing tests, and documented success-on-empty and no-index fallbacks in the USERGUIDE so run_js consumers know what to expect.
- **Expanded QuickJS tooling exposure**: Published the QuickJS `console` and `std/os` globals in `tools.help` and added run_js tests that verify the canonical helper list plus the accessible aliases.
- **Better Error reporting**: Delivered accurate runtime and syntax error line numbers and implemented `JS_EVAL_FLAG_COMPILE_ONLY` so compile-only evaluation no longer mutates runtime state.

## v0.12.1 - 2026-03-04

- **Orchestrator routing restored**: Provider selection now switches between OpenAI and Gemini based on the available credentials and uses matching API styles when routing responses or chat completions.
- **Default OpenAI model bumped**: The OpenAI provider now defaults to `gpt-5.1-codex-mini` as the recommended baseline.

## v0.12.0 - 2026-03-04

- **Removed Google OAuth and GCA integration**: Deprecated all Google OAuth flows and associated constants following updated Terms of Service restrictions.
- **Streamlined Authentication**: Updated `slop_auth.sh` and internal orchestration logic to focus on ChatGPT Plus/Pro for subscription-based model access.
- **Bug Fixes**: Fixed a syntax error in `slop_auth.sh` and cleaned up trailing newlines in core headers and source files.

## v0.11.0 - 2026-03-04

- **Improved reliability and safer defaults**: startup now fails fast when auth methods are missing, branch/shell guard behavior is clearer, and core defaults were updated for smoother out-of-box behavior (`update defaults`, 0673050).
- **Better efficiency and `run_js` workflow**: prompt/runtime updates improved **token efficiency**, added dynamic help/catalog support, and aligned the experience to a run_js-first flow.
- **Cleaner outputs and docs**: tool results now default to JSON formatting (including nested JSON), with docs/guides refreshed for the JS runtime migration.

# Changelog

## Changes since v0.1.9

### JavaScript control plane and tool model
- Consolidated model-facing tool usage around **`run_js`** for orchestration-first execution.
- Migrated JavaScript tools to a dynamic registry backed by **`js_functions`**, with manifest-driven discovery via `tools.help()`.
- Added dynamic help/catalog behavior and expanded schema metadata surfaced to the model.

### Runtime behavior and safety hardening
- Hardened JavaScript file/search helper behavior and tightened guardrails around patch-series workflows.
- Standardized tool result framing to JSON by default for more predictable downstream handling.
- Improved argument schema/type consistency across JS tools and helper wrappers.

### Prompt + bridge cleanup
- Updated prompts and orchestration guidance to align with the run_js-first workflow.
- Simplified JS preamble/runtime surfaces and removed legacy scratchpad/history globals from the bridge.

### Test and build updates
- Added/updated tests for dynamic help behavior, JS helper hardening, and migration compatibility.
- Updated JS tooling bundle/build wiring to match the new dynamic layout.


