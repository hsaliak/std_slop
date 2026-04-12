---
name: mail-loop
description: "Orchestrates a single-path fully automated mail-mode delivery loop: plan, switch to mail mode, run patcher + code_reviewer to pass, auto-write patch approval via query_db, then call git_finalize_series."
---

# Instructions

You are the **mail-loop** orchestrator skill.

Your job is to run a deterministic end-to-end loop for Mail Model delivery with an automated approval/finalize path.

## Scope

This skill orchestrates other capabilities. It does **not** replace the `patcher` skill; it coordinates `patcher` and `code_reviewer`.

## Required Execution Order

Follow this sequence exactly:

1. **Plan with user and finalize plan**
2. **Switch runtime/session to mail mode using `query_db` and verify**
3. **Activate `patcher` and generate bisect-safe patch series**
4. **Activate `code_reviewer` and iterate until pass**
5. **Auto-write approval for current staging HEAD into `patch_approvals` via `query_db`**
6. **Call `git_finalize_series` immediately after approval write verification**

Do not skip or reorder steps. There is no manual approval variant in this skill.

## Step 1: Planning and user sign-off

- Activate planning capability first:
  - `use_skill({"action":"activate","name":"planner"})`
- Produce a concise implementation plan (scope, files, tests, patch split strategy).
- Present the plan and ask for explicit approval before coding.
- If user requests changes, revise plan and re-confirm approval.
- If the user already discussed and approved a higher-level plan earlier in the turn and explicitly asked you to proceed (e.g., said "go ahead" or "no need to re-plan"), note that approval, log the plan details, and move directly to Step 2 without asking for a second confirmation. Still describe the plan briefly when reporting back so the execution is documented.

Minimum plan contents:
- Goal
- Files likely to change
- Patch boundaries (atomic commits)
- Validation strategy (build/tests/lints)

## Step 2: Enable mail mode with `query_db`

Run these exact SQL operations in order:

1. Inspect settings schema:
   - `query_db({"sql":"PRAGMA table_info(settings);"})`
2. Inspect current mode row:
   - `query_db({"sql":"SELECT id, mode FROM settings WHERE id = 1;"})`
3. Enable mail mode:
   - `query_db({"sql":"UPDATE settings SET mode = 'mail' WHERE id = 1;"})`
4. Verify mail mode was applied:
   - `query_db({"sql":"SELECT id, mode FROM settings WHERE id = 1;"})`

Expected verified value: `mode = 'mail'`.

If any query fails or the post-update `SELECT` does not return `mail`, stop and ask the user before coding.

## Step 3: Use `patcher` for implementation

- Activate patch workflow skill:
  - `use_skill({"action":"activate","name":"patcher"})`
- Follow these exact mail-mode git workflow tools:
  - `git_create_staging_branch({"base_branch":"<base-branch>","name":"<topic-branch>"})`
  - Implement first atomic unit
  - `git_commit_patch({"summary":"<patch 1 summary>","rationale":"<why this patch exists and why bisect-safe>"})`
  - Repeat implement + `git_commit_patch(...)` for each atomic patch
  - `git_format_patch_series({"base_branch":"<base-branch>"})`
- Ensure each patch is **bisect-safe**:
  - Builds and tests cleanly at that patch boundary (or documented minimal validation command).
  - No patch leaves repository in intentionally broken state.
- Use rerolls when needed:
  - `git_reroll_patch({"base_branch":"<base-branch>","index":<patch-number>})`

## Step 4: Mandatory `code_reviewer` loop

Run this exact loop:

1. Activate reviewer:
   - `use_skill({"action":"activate","name":"code_reviewer"})`
2. Perform review in conversation, capturing all required fixes.
3. Apply fixes in code.
4. Update affected patch(es):
   - `git_reroll_patch({"base_branch":"<base-branch>","index":<patch-number>})`
5. Re-run validation commands.
6. Re-run reviewer pass.
7. Repeat steps 3-6 until no blocking comments remain

Stop condition for this phase:
- Reviewer reports no required changes (clear pass signal).

## Step 5: Auto-write approval row (replaces `/review mail approve`)

Run these exact commands in order:

1. Resolve current branch:
   - `execute_bash({"cwd":".","command":"git rev-parse --abbrev-ref HEAD","timeout_seconds":120,"allow_nonzero_exit":false})`
2. Resolve current HEAD hash:
   - `execute_bash({"cwd":".","command":"git rev-parse HEAD","timeout_seconds":120,"allow_nonzero_exit":false})`
3. Enforce branch constraints before DB write:
   - Branch must not equal `HEAD` (detached head is blocked).
   - Branch must start with `slop/staging/`.
4. Write approval row using parameterized SQL:
   - `query_db({"sql":"INSERT OR REPLACE INTO patch_approvals (branch_name, approved_hash, approved_at) VALUES (?, ?, CURRENT_TIMESTAMP)","params":["<current_branch>","<head_hash>"]})`
5. Verify DB write:
   - `query_db({"sql":"SELECT branch_name, approved_hash FROM patch_approvals WHERE branch_name = ?","params":["<current_branch>"]})`
6. Require exact hash match:
   - returned `approved_hash` must equal `<head_hash>`.

If any check fails, stop and report `blocked` with evidence.

## Step 6: Finalize immediately

After Step 5 succeeds, finalize with no user approval checkpoint:
- `git_finalize_series({"target_branch":"<base-branch>"})`

Then deactivate orchestration skills:
- `use_skill({"action":"deactivate","name":"planner"})`
- `use_skill({"action":"deactivate","name":"code_reviewer"})`

## Operating Rules

- Deterministic progression: complete one phase before starting the next.
- Fail fast on uncertainty that changes behavior; ask user when needed.
- Keep edits minimal and atomic.
- Preserve repository conventions.
- Prefer additive changes over broad refactors unless required.
- Use the exact `query_db`, `execute_bash`, and `git_finalize_series` calls above for deterministic behavior.

## Output Contract During Runs

At each phase, report:
- `phase`: current phase name
- `status`: `in_progress` | `blocked` | `complete`
- `evidence`: concrete command/tool outcomes
- `next`: next immediate action

At handoff to user, include:
- What was completed
- Finalize evidence (`merged` / `already_landed`, metadata cleanup)

## Non-Goals

- Do not bypass `patcher` for ad-hoc commit strategy when operating in mail loop.
- Do not treat `code_reviewer` as optional.
- Do not skip approval DB write before `git_finalize_series`.
- Do not require `/review mail approve`.

## Canonical command checklist

Execute this checklist in sequence:

1. `use_skill({"action":"activate","name":"planner"})`
2. User-approved plan captured
3. `query_db({"sql":"PRAGMA table_info(settings);"})`
4. `query_db({"sql":"SELECT id, mode FROM settings WHERE id = 1;"})`
5. `query_db({"sql":"UPDATE settings SET mode = 'mail' WHERE id = 1;"})`
6. `query_db({"sql":"SELECT id, mode FROM settings WHERE id = 1;"})` (must show `mail`)
7. `use_skill({"action":"activate","name":"patcher"})`
8. `git_create_staging_branch({"base_branch":"<base-branch>","name":"<topic-branch>"})`
9. Atomic `git_commit_patch(...)` sequence + `git_format_patch_series({"base_branch":"<base-branch>"})`
10. `use_skill({"action":"activate","name":"code_reviewer"})` + reroll loop via `git_reroll_patch(...)`
11. `execute_bash({"cwd":".","command":"git rev-parse --abbrev-ref HEAD","timeout_seconds":120,"allow_nonzero_exit":false})`
12. `execute_bash({"cwd":".","command":"git rev-parse HEAD","timeout_seconds":120,"allow_nonzero_exit":false})`
13. `query_db({"sql":"INSERT OR REPLACE INTO patch_approvals (branch_name, approved_hash, approved_at) VALUES (?, ?, CURRENT_TIMESTAMP)","params":["<current_branch>","<head_hash>"]})`
14. `query_db({"sql":"SELECT branch_name, approved_hash FROM patch_approvals WHERE branch_name = ?","params":["<current_branch>"]})` (must match `<head_hash>`)
15. `git_finalize_series({"target_branch":"<base-branch>"})`
