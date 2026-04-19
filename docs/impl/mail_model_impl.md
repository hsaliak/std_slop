# Implementation Plan: The Mail Model

> Implementation/design-history note: this document is not the primary user guide. For the current user-facing workflow, see [../mail_mode.md](../mail_mode.md).

This document captures the original implementation plan for the Mail Model workflow in `std::slop`.

## Scope of the Plan

The implementation was broken into these major areas:

1. Core Git tooling for staging branches, patch commits, formatting, rerolls, verification, and finalize.
2. Review UI support for `/review mail` and patch-index-specific review.
3. Agent-skill support for patch-oriented contribution loops.
4. Persistence of staging-branch metadata and series context.

## Early Tooling Outline

The original design included tooling for:
- staging-branch creation,
- patch commit creation,
- rerolls via fixup/rebase,
- formatting a mail-style patch series,
- review flows,
- finalization.

Some names in this document reflect the design phase and may differ from the current runtime tool names.

## Historical Notes

This file is kept as design history for the evolution of Mail Mode. For current usage guidance, use [../mail_mode.md](../mail_mode.md) and [../mail-loop/README.md](../mail-loop/README.md).
