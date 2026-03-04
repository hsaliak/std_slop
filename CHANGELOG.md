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


