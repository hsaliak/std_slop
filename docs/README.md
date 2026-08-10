# Documentation Guide

Use this index to find the right `std::slop` doc for your task.

## Start Here

- [WALKTHROUGH.md](WALKTHROUGH.md) — canonical onboarding guide for installation, authentication setup, `config.ini`, first session flow, and `llm_query` subquery configuration.
- [OAUTH.md](OAUTH.md) — OpenAI OAuth token bootstrap, runtime behavior, token refresh, and token path overrides.
- [README.md](../README.md) — project overview, build instructions, and a high-level docs map.

## Advanced Workflows

- [mail_mode.md](mail_mode.md) — manual-review-first patch workflow using small, discrete, bisectable staging-branch patches, review, rerolls, and finalization.
- [mcp-slop-userguide.md](mcp-slop-userguide.md) — configure and use MCP servers from `std_slop`.

## Core Concepts and Reference

- [CONTEXT.md](CONTEXT.md) — global context injection, personas, and skills.
- [SESSIONS.md](SESSIONS.md) — session isolation, persistence, and cloning behavior.
- [SCHEMA.md](SCHEMA.md) — database-driven architecture and schema reference.
- [CONTEXT_MANAGEMENT.md](CONTEXT_MANAGEMENT.md) — history/windowing strategy.
- [CONTRIBUTING.md](CONTRIBUTING.md) — code style, formatting, and contribution guidance.
- [fuzzing.md](fuzzing.md) — fuzz targets, invariants, and maintenance guidance.
- [mcp-api.md](mcp-api.md) — reusable C++ MCP client library surface, Streamable HTTP behavior, bearer tokens, and OAuth helpers.

## Example Config Files

- [example_config.ini](example_config.ini) — baseline configuration template.
- [example_subqueries.ini](example_subqueries.ini) — example INI sections for specialized `llm_query` tools.

## Implementation Reference

- [impl/subqueries.md](impl/subqueries.md) — INI-defined `llm_query` tool contract.

## Reading Order

1. Start with [WALKTHROUGH.md](WALKTHROUGH.md).
2. Use [OAUTH.md](OAUTH.md) if you need OpenAI OAuth specifics.
3. Use [mail_mode.md](mail_mode.md) for patch-based workflows.
4. Use the reference documents as needed.
