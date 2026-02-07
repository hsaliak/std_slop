# Implementation Plan: The Mail Model

This document outlines the step-by-step technical implementation of the Mail Model workflow in `std::slop`.

## Phase 1: Core Git Tooling (The Engine)
The goal of this phase is to provide the agent with the low-level primitives to manage a staging environment.

### 1.1 `git_branch_staging(name, base_branch)`
- **Task**: Create a new tool that initializes a staging branch.
- **Requirements**:
    - Must ensure the repository is clean before starting.
    - Default `base` to the current branch if not provided.
    - Follow naming convention: `slop/staging/<name>`.
- **Validation**: Test that it correctly branches and handles "dirty" state errors.

### 1.2 `git_commit_patch(summary, rationale)`
- **Task**: Create a structured commit tool.
- **Logic**:
    - Constructs a commit message: `Summary\n\nRationale: <rationale>`.
    - Automatically adds all changes in the workspace (`git add .`).
    - Enforces the inclusion of the "Rationale" field.
- **Validation**: Verify that `git log` shows the rationale correctly formatted.

---

## Phase 2: Verification & The Series Walk
Ensuring that every commit in the series is functional.

### 2.1 `git_verify_series(command, base_branch)`
- **Task**: Implement the "Series Walk" tool.
- **Logic**:
    1. Identify all commits on the current staging branch that aren't on the base branch.
    2. For each commit:
        - `git checkout <hash>`
        - Execute `command` (e.g., `bazel build //...`). Note this command must be extensible to the project std::slop is working on.
        - Capture success/failure.
    3. Return to the tip of the staging branch.
    4. Return a structured report (Markdown) of the results.
- **Validation**: Create a 3-patch series where the 2nd patch is broken; verify the tool identifies it.

---

## Phase 3: The Reroll Engine
Automating the surgical update of the Git history.

### 3.1 `git_reroll_patch(index, base_branch)`
- **Task**: Implement the automated fixup/rebase logic.
- **Logic**:
    1. Map `index` (1-based) to the corresponding commit hash in the series.
    2. Performs `git add .` on the current workspace.
    3. Create a fixup commit: `git commit --fixup <hash>`.
    4. Execute non-interactive rebase: `GIT_SEQUENCE_EDITOR=true git rebase -i --autosquash <base_branch>`.
- **Validation**: Verify that a series of 3 patches remains 3 patches after a reroll of index 1.

---

## Phase 4: Formatting & Presentation
Converting Git history into a human-readable "Mail Package."

### 4.1 `git_format_patch_series(base_branch)`
- **Task**: Extract rationale and diffs into a structured format for the LLM.
- **Logic**:
    - Extract summaries and "Rationale" blocks from the commit history.
    - Generate a "Cover Letter" summary.
    - Generate unified diff strings for each patch.
- **Validation**: Ensure rationales are correctly extracted even if they span multiple lines.

---

## Phase 5: UI & CLI Integration
Connecting the tools to the user interface.

### 5.1 Extension of `/review`
- **Subcommand `/review patch`**: 
    - Fetch the entire series.
    - Inject `### Patch [n] ###` headers into a temporary buffer.
    - Open the user's editor.
- **Subcommand `/review patch <index>`**:
    - Open only the specific patch in the editor.
- **The Inlined `R:` Protocol**: 
    - The agent is instructed to scan the returned buffer for `R:` comments *inlined* under patch headers (e.g., `### Patch [n/total] ###`).
    - An `R:` comment under `### Patch [2/3] ###` automatically signals that the changes must be applied to the 2nd patch in the series using `git_reroll_patch(index=2)`.

### 5.2 `/mode mail` Toggle
- **Logic**:
    - Check if the current directory is a Git repo; if not, prompt `git init`.
    - Toggle the `mail_mode` flag in the session state.
    - Update the UI Modeline to reflect the current mode.
    - Activate/Deactivate the `patcher` skill.

---

## Phase 6: The Patcher Skill (LLM Logic)
Defining the persona and intent detection.

### 6.1 The `patcher` System Prompt
- Define a system prompt that:
    - Sets the "Remote Contributor" persona.
    - Instructs the model to use the Stage 1-4 tools exclusively for modifications.
    - Explains how to interpret `R:` comments from the review tool.

### 6.2 "LGTM" Intent Detection
- **Logic**:
    - Delegate intent detection to the LLM via the `patcher` skill's system prompt.
    - The `patcher` persona is explicitly instructed to call `git_finalize_series` when the user provides positive feedback (e.g., "LGTM", "Looks good", "Approved").
    - This allows for more flexible, natural language interpretation of approval without hardcoded keywords.

---

## Phase 7: Dynamic Base Branch & Persistence
Ensuring the workflow is robust across different repository structures and session restarts.

### 7.1 Metadata Persistence (`slop.basebranch`)
- **Mechanism**: Use `git config slop.basebranch <name>` to store the target branch for a staging series.
- **Logic**:
    1. `git_branch_staging` captures the current branch (e.g., `main`, `develop`, or a feature branch) and stores it in the local git config.
    2. All subsequent tools (`git_format_patch_series`, `git_verify_series`, `git_reroll_patch`, `git_finalize_series`) use a centralized `GetBaseBranch()` helper.
- **Resolution Order**:
    1. Explicit `base_branch` argument (if provided).
    2. `slop.basebranch` from git config.
    3. Auto-detected `main` or `master`.
    4. Fallback to `origin/main` or `origin/master`.

### 7.2 Diagnostics & UX
- **No-Patch Messaging**: If `base..HEAD` is empty, the system checks if the user is currently on the base branch and provides a contextual tip.
- **Transparency**: Commands like `/review patch` and `/mode mail` now display the detected base branch to avoid ambiguity.
- **Auto-Cleanup**: `git_finalize_series` unsets the `slop.basebranch` config after a successful merge.

---

## Success Criteria
1. **Atomic History**: A feature developed in Mail Mode results in a series of logical commits in the main branch.
2. **Bisect-Safe**: Every commit in that series compiles and passes tests.
3. **Traceability**: Every commit has a clear rationale recorded in its body.
4. **Clean Exit**: Merging the series cleans up the staging branch and returns the user to a clean workspace.
