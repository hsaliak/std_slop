# Documentation Guide

Use this index to find the right `std::slop` doc for your task.

## Start Here

- [WALKTHROUGH.md](WALKTHROUGH.md) — canonical onboarding guide for installation, authentication setup, `config.ini`, first session flow, and `llm_query` subquery configuration.
- [OAUTH.md](OAUTH.md) — OpenAI OAuth token bootstrap, runtime behavior, token refresh, and token path overrides.
- [README.md](../README.md) — project overview, build instructions, and a high-level docs map.

## Advanced Workflows

- [mail_mode.md](mail_mode.md) — manual patch-based workflow using staging branches, patch commits, review, rerolls, and finalize.
- [mail-loop/README.md](mail-loop/README.md) — human-oriented overview of the automated mail-loop persona.
- [mail-loop/SKILL.md](mail-loop/SKILL.md) — the precise operational contract for the `mail-loop` skill.

## Core Concepts and Reference

- [CONTEXT.md](CONTEXT.md) — global context injection, personas, and skills.
- [SESSIONS.md](SESSIONS.md) — session isolation, persistence, and cloning behavior.
- [SCHEMA.md](SCHEMA.md) — database-driven architecture and schema reference.
- [CONTEXT_MANAGEMENT.md](CONTEXT_MANAGEMENT.md) — history/windowing strategy.
- [CONTRIBUTING.md](CONTRIBUTING.md) — code style, formatting, and contribution guidance.
- [fuzzing.md](fuzzing.md) — fuzz targets, invariants, and maintenance guidance.

## Example Config Files

- [example_config.ini](example_config.ini) — baseline configuration template.
- [example_subqueries.ini](example_subqueries.ini) — example INI sections for specialized `llm_query` tools.

## Implementation Notes and Design History

These files live under [`docs/impl/`](impl/) and are useful when you want design rationale, implementation notes, or historical planning context rather than end-user guidance.

- [impl/config_impl.md](impl/config_impl.md)
- [impl/subqueries.md](impl/subqueries.md)
- [impl/mail_model_impl.md](impl/mail_model_impl.md)
- [impl/tree-sitter-plans.md](impl/tree-sitter-plans.md)

## Reading Order

1. Start with [WALKTHROUGH.md](WALKTHROUGH.md).
2. Use [OAUTH.md](OAUTH.md) if you need OpenAI OAuth specifics.
3. Use [mail_mode.md](mail_mode.md) or [mail-loop/README.md](mail-loop/README.md) for patch-based workflows.
4. Use the reference and implementation docs only when you need deeper project internals.
