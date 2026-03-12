
# Mail Loop Skill Docs

This directory documents the **mail-loop** skill.

`mail-loop` is an automated loop around Mail Mode that drives a full patch workflow:

1. Plan and confirm scope with the user.
2. Switch into mail mode and create/use a staging branch.
3. Build a bisect-safe patch series with atomic commits.
4. Run verification and review/reroll iterations until clean.
5. Present the series for explicit user approval.
6. Finalize only after approval for the exact reviewed HEAD.

Copy these into `.agents/skills/mail-loop/` to use or to ~/.config/slop/skills/
