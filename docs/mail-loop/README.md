# Mail Loop Skill Docs

This directory documents the **mail-loop** skill.

**Audience:** users who want an automated patch-series workflow built on top of Mail Mode.
**Related docs:** [../mail_mode.md](../mail_mode.md), [../README.md](../README.md), [SKILL.md](SKILL.md)

`mail-loop` is an automated loop around Mail Mode that drives a full patch workflow:

1. Plan and confirm scope with the user.
2. Switch into mail mode and create/use a staging branch.
3. Build a bisect-safe patch series with atomic commits.
4. Run verification and review/reroll iterations until clean.
5. Run the mandatory reviewer/reroll loop until the series is clean.
6. Write the approval row automatically for the current reviewed HEAD and finalize immediately.

## When to use mail-loop

Use `mail-loop` when you want the agent to coordinate the patch workflow deterministically instead of manually driving each Mail Mode step yourself.

## Manual Mail Mode vs mail-loop

- Use [../mail_mode.md](../mail_mode.md) when you want to control each patch step manually.
- Use `mail-loop` when you want a higher-level orchestrator to handle planning, patch creation, reviewer iteration, approval bookkeeping, and finalization flow.

## Installation

Copy these files into one of:

- `.agents/skills/mail-loop/`
- `~/.config/slop/skills/mail-loop/`

The exact behavior is defined in [SKILL.md](SKILL.md).
