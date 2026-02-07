# The Mail Model: Patch-Based Development Workflow

This document describes the "Mail Model" workflow for `std::slop`, a Git-native, iteration-first development process inspired by the Linux Kernel / Git email workflow.

## 1. Core Philosophy
The agent acts as a **Remote Contributor**. Instead of directly modifying the project's main files, it submits a **Patch Series** for review. This forces architectural thinking, atomic changes, and a clear audit trail of design decisions.

## 2. The Four Stages

### Stage 1: The Staging Layer (The Sandbox)
- **Branching**: All work happens on a dedicated staging branch (e.g., `slop/staging/feature-name`) branched from the current HEAD.
- **Atomic Commits**: Every logical change (e.g., "Defined Interface", "Added Implementation", "Updated Tests") is a separate Git commit.
- **Rationale Capture**: Each commit contains a "Rationale" block in the body, explaining *why* the change was made, not just *what* changed.
- **Patch Hygiene (Verification)**: 
    - The agent MUST run build/test commands (e.g., `bazel build //...`) after each commit.
    - If a commit fails the build, the agent must fix it *before* proceeding to the next patch or presenting the series. This ensures the series is "Bisect-Safe."

### Stage 2: The Patch Series (The Review)
- **The Package**: The agent generates a series of patches using `git format-patch`.
- **The Cover Letter**: The agent summarizes the entire series in the chat.
- **Review Interface (The `/review` Flow)**:
    - **Automated Summary**: A Markdown table listing patch indices, summaries, and rationales.
    - **`/review mail`**: Opens the *entire* patch series in the editor for review. Patch headers (e.g., `### Patch [1/2] ###`) are injected into the buffer to partition the changes.
    - **`/review mail <index>`**: Opens only the specified patch in the editor.
- **Feedback Methods**:
    - **Inlined Reworks**: Users add comments starting with `R: ` directly under the lines they want changed in the review editor.
    - **Contextual Rerolling**: The agent uses the `### Patch [n/total] ###` headers to automatically map `R:` comments to the correct atomic commit. If an `R:` comment is found under Patch 2's header, the agent will apply the fix to Patch 2 using `git_reroll_patch(index=2)`.
    - **Direct Chat**: Chat-based feedback is still supported for high-level requests.
- **Review Boundary**: The agent stops and waits for user feedback before landing any code.

### Stage 3: The Reroll (Iteration)
- **Feedback Parsing**: The agent analyzes user feedback (Chat or `/review`) and maps it to specific patches in the current series.
- **The "Fixup" Loop**:
    1.  Agent applies corrections to the staging branch.
    2.  Agent creates fixup commits (`git commit --fixup <patch_hash>`).
    3.  Agent performs an interactive rebase (`git rebase -i --autosquash`) to maintain atomic patch integrity.
    4.  **Re-Validation**: After the rebase, the agent performs a "Series Walk." It iterates through each commit in the updated series and runs build/test commands to ensure the rebase didn't introduce regressions or logical conflicts in subsequent patches.
- **Version Tracking**:
    - The series is incremented (e.g., `v1` -> `v2`).
    - The new Cover Letter includes a **Changelog** section (e.g., "v2: Fixed memory leak in Patch 2; updated naming in Patch 1").
- **State Persistence**: The iteration history is stored in the Git Reflog and the commit bodies, allowing for a full "Undo" if a reroll goes in the wrong direction.

### Stage 4: Finalization (Landing)
- **Acceptance**: Once the user approves, the agent lands the changes.
- **Integration**: The agent performs a `merge` (typically `--squash` or `--ff-only`) into the target branch.
- **Cleanup**: The staging branch is deleted, leaving the workspace clean.

## 3. Workflow Example: "Add a FileCache"

1. **Agent Setup**: 
   - Agent calls `git_branch_staging(name="file-cache")`.
2. **Commit 1 (API Design)**:
   - Agent modifies `interface/cache.h`.
   - Agent calls `git_commit_patch(summary="interface: define ICache", rationale="Abstract interface allows us to swap local storage for S3 later.")`.
3. **Commit 2 (Implementation)**:
   - Agent modifies `core/local_cache.cpp`.
   - Agent calls `git_commit_patch(summary="core: implement LocalCache", rationale="Simple std::filesystem-based implementation for initial MVP.")`.
4. **The "Mail" (Presentation)**:
   - Agent calls `git_format_patch_series()` and presents the work to the user.
5. **Review**:
   - **User** runs `/review mail`.
   - In the editor, under `### Patch [1/2] ###`, user adds: `R: Use std::string_view for keys here.`
6. **Reroll**:
   - Agent applies the code change to `interface/cache.h`.
   - Agent calls `git_reroll_patch(index=1)`.
   - Agent calls `git_verify_series(command="bazel test //...")`.
   - Agent presents updated series to the user.
7. **Finalize**:
   - **User**: "LGTM."
   - Agent calls `git_finalize_series()`. The staging branch is merged and deleted.

## 4. Required Tooling & Commands

### Mode Activation
- **`/mode mail`**: Enables the "Mail Model" persona. This activates the `patcher` skill and injects the corresponding system prompt. Be sure to provide a visual update to the modeline
- **Session Toggle**: This mode can be toggled on/off. When off, the agent reverts to direct file modification. When on, all modifications must go through the staging/patch loop.
- The Mode state default vs mail is shown in the modeline of the UI.
- When the /mode mail is toggled in a directory that is not a valid git repository, it should ask the user to git init, if not, it should not toggle the mode. 

### CLI Commands (User Interface)
- **`/review mail`**: Opens the current patch series in the review editor.
- **`/review mail <index>`**: Opens a specific patch for detailed inspection and `R:` commenting.
- **Diagnostics**: The review system provides contextual tips:
    - Running `/review` without changes in Mail Mode will suggest `/review mail`.
    - Running `/review mail` without commits on a staging branch will suggest using `git_commit_patch`.

### Finalization (The "LGTM" Flow)
- **Conversational Approval**: There is no mandatory `/approve` command. Instead, the agent detects the user's intent to finalize from the chat (e.g., "LGTM", "Ship it", "Looks good, merge it").
- **Review Tool Signal**: If the user closes the `/review` buffer without adding any `R:` comments, the agent will prompt: "I see no comments in the review. Should I land this patch series?"

### Agent Tools (The Engine)
- **`git_branch_staging(name, base_branch)`**: Initializes the staging environment.
- **`git_commit_patch(summary, rationale)`**: Commits a logical change with mandatory metadata.
- **`git_format_patch_series(base_branch)`**: Returns the formatted cover letter, changelog, and a list of unified diffs for the LLM to present.
- **`git_reroll_patch(index, base_branch)`**: A high-level tool that handles the `fixup` + `rebase` logic. It incorporates current workspace changes into a specific patch in the series.
- **`git_verify_series(command, base_branch)`**: Automates the "Series Walk." It checks out each commit in the current series sequentially and runs the provided build/test command. It returns a report of which patches passed or failed.
- **`git_finalize_series(target_branch)`**: Merges the staging branch into the target branch and cleans up.

### The `patcher` Skill
- A system prompt that:
    1.  Prohibits `write_file` on protected branches.
    2.  Forces the use of the `git_commit_patch` -> `git_format_patch_series` loop.
    3.  Instructions on how to generate high-quality Cover Letters and Changelogs.
    4.  Provides logic for mapping `R:` comments from specific patch buffers back to the correct Git commit for rerolling.
