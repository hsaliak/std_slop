## v0.22 - 2026-06-17
- **Persisted `run_js` helpers fixed**: Made `tools.persist_function(args)` callable from `run_js`, safely validates helpers before storage, and loads persisted helpers into later `run_js` invocations so reusable JavaScript helpers such as `runCommandSummary` work end-to-end.
- **Persisted helper safety checks**: Added validation for helper names, JSON schema metadata, optional `test_args`, size limits, global-name collisions, no-mutation failure paths, and regression coverage for malformed or unsafe helper definitions.

## v0.20.1 - 2026-06-16
- **Persisted helper learning guidance**: Updated the system prompt to encourage promoting repeated reusable `run_js` orchestration patterns into `tools.persist_function(args)`, making deterministic JavaScript helpers part of the long-term self-learning loop and discoverable through `tools.help()` or `/tools js_help`.

## v0.20 - 2026-06-16
- **QuickJS `run_js` control plane**: Re-Added a top-level `run_js` tool backed by QuickJS for bounded, synchronous JavaScript control flow. The bridge validates untrusted arguments, returns JSON results, reports syntax/runtime/serialization failures as statuses, and includes unit/fuzz coverage for malformed scripts and call shapes. std::slop is back to being a js runtime oriented coding agent!
- **ToolExecutor-backed JavaScript helpers**: Routed JavaScript `call_tool` and `tools.*` helper calls through the normal ToolExecutor path so validation, permissions, and subquery policy are preserved. Runtime helper discovery now comes from `tools.help()`, `/tools js_help`, and the generated shared bootstrap source while recursive/workflow-tool dispatch remains blocked.
- **Dense editing and shell workflows**: Replaced the old `patch_tool` flow with exact-text `edit_tool` semantics designed for dense `run_js` snippets, added helpers for file writes, patch-style edits, and focused shell validation, and documented compact multi-helper patterns for read/edit/test loops.
- **Quote-safe `run_js` input payloads**: Added optional `{ "input": ... }` support exposed as `globalThis.input`, allowing source-code and edit payload strings to be passed as JSON data instead of embedded in JavaScript literals. Tests cover default input, nested passthrough, hostile quote-heavy strings, edit payload forwarding, schema visibility, and fuzz coverage.
- **Prompt, UI, and documentation polish**: Tightened system prompt guidance around deterministic `run_js` usage, scratchpad state, dispatch validation, result-size limits, repo-relative file paths, and safe shell command construction. The UI now renders `run_js` code with JavaScript syntax highlighting and summarizes large tool results, while README/docs were refreshed for helper discovery, persisted helpers, and input-based edit examples.

## v0.19 - 2026-06-01
- **Batch prompt mode**: Added non-interactive prompt execution with `--prompt` and `--prompt-file`, machine-readable `--output json`, prompt-mode tests, and README guidance. Piped stdin is now used automatically as contextual input before the explicit instruction, and the release removes the earlier `--prompt-stdin`/`--prompt=-` style inputs in favor of one clean prompt contract.
- **Session workflow improvements**: Added grouped session switch and rollback commands, preserved arbitrary session names during switching, and replaced the compact-session path with LLM-generated session summaries plus inline one-shot calls for compact summaries.
- **OpenAI Responses reliability**: Normalized final Responses SSE output and disabled streaming for one-shot OpenAI Responses calls so prompt/summary flows receive stable final text.
- **Release and automation cleanup**: Removed legacy `slop_auth` helper scripts, split CLI help from in-app help rendering, refined mail-loop finalization behavior, refreshed the mail-loop skill, and added post-task learning capture guidance for future maintenance.

## v0.18.1 - 2026-04-04
- **Documentation refresh and restructuring**: Made `docs/WALKTHROUGH.md` the canonical onboarding guide, added `docs/README.md` as a docs index, and aligned `README.md` to point users at the new walkthrough-first flow.
- **Docs cleanup**: Removed the redundant `docs/USERGUIDE.md`, moved implementation/history material into `docs/impl/`, and refreshed cross-links so user-facing docs and design-history docs are clearly separated.
- **Workflow docs**: Refreshed Mail Mode, mail-loop, OAuth, contributing, and subquery documentation to reflect current tool names, current mail-loop behavior, and the latest authentication/configuration guidance.

## v0.18 - 2026-04-04
- **OpenAI OAuth bootstrap + CLI cleanup**: Added built-in OpenAI OAuth bootstrap flows for both browser/manual and headless device-code login, improved device-flow retry and backoff behavior, and cleaned up the CLI by renaming the bootstrap flags to `--fetch_openai_oauth_token` and `--fetch_openai_oauth_device_token`. `--help` and `--helpfull` now focus on command-line options only, without Hotword or Slash Command sections. This release also updates related docs/helper guidance, moves PKCE SHA-256 generation into a dedicated helper with tests, and includes a small input-parsing fuzz fix.

## v0.17.3 - 2026-04-04
- **Fix CI-CD workflow**: update yaml file

## v0.17.2 - 2026-04-04
- **Documentation**: Updated architecture/config docs to reference `app/main.cpp` instead of the old root-level `main.cpp` path.

## v0.17.1 - 2026-04-04
- **Subquery infinite-context support (`context_window = 0`)**: For context window configuration of llm_query subagents, 0 now means infinite, similar to how /context window 0 means infinite. This is useful for certain workflows.  `0` now explicitly means infinite history for delegated subqueries; added unit/fuzz coverage and updated subquery docs/examples accordingly.


## v0.17 - 2026-04-04
- **Config-defined `llm_query` sub-agents**: New feature - added INI-driven specialization loading for `llm_query` via `[llm_tool_<name>]` sections with required field validation (`system_prompt_patch`, `session_id`, `skill`) and optional `context_window`. These allow sub agetns to have specialized and persistent context windows that can be delegated into. 
- **Startup registration for specializations**: Added startup wiring that registers each specialization as a first-class tool and keeps stale specialization tools in sync with active config.
- **Delegation API and policy hardening**: Generalized `llm_query` execution options and centralized subquery policy checks to block recursion paths (`llm_query`/`llm_tool_*` in subquery scope) with fixed depth constraints.
- **Fuzz + test coverage for subquery paths**: Added dedicated fuzz targets for specialization config parsing, subquery policy boundaries, and query options handling; expanded unit coverage around registration/validation behavior.
- **Naming alignment to valid tool identifiers**: Standardized specialization naming and docs/tests from dot form to underscore form (`llm_tool_...`) to match tool-name constraints.
- **Docs discoverability improvements**: Added/expanded subquery documentation, `docs/example_subqueries.ini`, and prominent onboarding/reference guidance for configuring and invoking specialization tools.

## v0.16 - 2026-03-17
- **Tooling split and API cleanup**: Moved the tool executor/dispatcher and built-in tool handlers out of `core/` into top-level `tools/`, then rewired the root, interface, and core build graph around new façade targets and shared dependencies.
- **Fuzzing coverage expansion**: Added FuzzTest targets for core JSON helpers, dispatcher validation, orchestrator normalization, tool-argument validation, and interface input parsing to harden boundary code against malformed input.
- **OpenAI default model update**: Bumped the default OpenAI model to `gpt-5.4-mini:high` for both standard OpenAI usage and Responses API routing.
- **Docs and contributor guidance**: Added a dedicated fuzzing guide, linked it from the README, and tightened AGENTS.md guidance around JSON helpers, validation boundaries, and test selection.

## v0.15.4 - 2026-03-12
- **Runtime and network efficiency improvements**: Reduced shell command polling overhead with bounded poll slices and reusable `pollfd` storage, reused fenced-block Markdown parsers during injection parsing, and moved libcurl global setup to a process-wide `absl::call_once` path.
- **Database history-path performance and safety**: Added targeted SQLite indexes for conversation-history and grouped-message lookups, capped prepared-statement cache growth, and removed destructive startup cleanup of the `code_search` table.
- **Sharper interactive UX and mail tooling**: Persisted readline history with better wrapped-input behavior, refreshed prompt mode and active-skill state each turn, and added native `git_verify_series` support plus a more automated mail-loop flow.

## v0.15.3 - 2026-03-12
- **OpenAI Responses duplication fix**: Prevented duplicate final assistant messages when SSE streams include both `response.output_text.delta` and `response.output_item.done` output text; added a regression test for this flow.
- **`execute_bash` timeout plumbing**: Added explicit `timeout_seconds` wiring with a 180s default and structured timeout handling to improve reliability and reporting.
- **Diff rendering polish**: Improved unified-diff markdown highlighting and UI diff rendering behavior.
- **Dependency maintenance**: Refreshed `tree-sitter-unified-diff` `master.tar.gz` checksum in `MODULE.bazel` to match the current upstream archive.
- **Maintenance**: Included lint/style follow-up cleanup commits.

## v0.15.2 - 2026-03-11
- **Mail mode UX and flow improvements**: Added `/mode mail <branchname>` auto-staging with validation, then refined mail model smoothness and formatting through follow-up rerolls.
- **Scratchpad support restored**: Reintroduced the session scratchpad table plus commands/tools, and followed up with scratchpad-focused fixes and README guidance.
- **Patch tool output rendering**: Added markdown diff rendering for `patch_tool` unified diffs in the UI while preserving existing truncation behavior.
- **Tooling/docs cleanup**: Removed an unnecessary tool, updated `/mode` help text, and renamed mail model docs to `mail_mode` for consistency.
- **Maintenance**: Applied style and clang-format cleanup commits.

## v0.15.1 - 2026-03-11
- **Gemini Integration**: Normalized Gemini tool schemas and added union-type regression tests to ensure robust tool calling.
- **Tooling Improvements**: Increased the `git_commit_patch` summary limit to 500 characters to allow for more descriptive commit messages.
- **Documentation**: Refreshed the README and removed obsolete JavaScript control plane references.
- **Maintenance**: Fixed clang-tidy `qualified-auto` warnings in schema traversal and performed general code cleanup.

## v0.15 - 2026-03-10
- Removed JavaScript control-plane migration artifacts: This is unfortunate, but models are RLed so hard, that such an approach is inferior. QuickJS-NG is an excellent project to integrate with but we must stay on our path.
- Completed post-migration prompt/skill cleanup to align with the native tool interface and current mail-model git tool names.
- Confirmed no residual JS-control-plane anchors remain (`run_js`, `persist_function`, `js_functions`, `js-bridge`, `generate_js_tools`, `generate_js_preamble`, `GetDefaultJsFunctions`).

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


