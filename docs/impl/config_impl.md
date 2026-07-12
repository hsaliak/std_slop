# INI Configuration Implementation Notes

> Implementation/design-history note: this document is not the primary user guide. For current setup instructions, start with [../WALKTHROUGH.md](../WALKTHROUGH.md).

This document captures the implementation plan and design rationale for adding INI configuration support to `std_slop`.

## Goals
- Allow users to persist settings in an INI file.
- Support a default config location at `~/.config/slop/config.ini`.
- Ensure a clear precedence order: CLI arguments > INI file > Environment variables > hardcoded defaults.

## Main Integration Outline

### 1. Configuration Loader
- Add a config loader in `core/` that:
  - resolves the config path (explicit `--config` or default `~/.config/slop/config.ini`),
  - parses the INI file,
  - applies values to `absl` flags,
  - updates flags only when they were not explicitly set on the command line.

### 2. Documentation Artifacts
- `../example_config.ini`: baseline template with supported keys.
- `../example_subqueries.ini`: example INI sections for specialized `llm_query` tools.

### 3. Runtime Integration
- Add `--config` as a flag.
- Call config loading early in `main()` after command-line parsing.
- Keep provider conflict resolution in runtime logic rather than in the loader.

## Precedence Order
1. Command line arguments
2. INI file
3. Environment variables
4. Hardcoded defaults

## Notes

Settings like `openai_api_key` and `openai_oauth` remain governed by runtime selection logic in `app/main.cpp` and `core/orchestrator.cpp`. The INI loader populates flags, but conflict resolution stays in runtime code.

See [../example_subqueries.ini](../example_subqueries.ini) for a working specialization example.
