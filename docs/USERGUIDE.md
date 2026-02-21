# std::slop User Guide

## Overview
`std::slop` is a C++ LLM CLI built for developers who want a SQL-backed, persistent conversation history with built-in tools for codebase exploration and context management.

## The Lua Control Plane

`std::slop` uses Lua 5.5 to orchestrate tool calls. This allows the agent to handle complex logic, loops, and parallel execution without multiple model round-trips for every small step.

### `run_lua`
The primary tool used by the agent. It takes a `script` string. The LLM will implement your request by writing Lua scripts. For detailed documentation on orchestration patterns, parallel execution, and the RLM paradigm, see [lua_integration.md](lua_integration.md).


## Installation
Build using Bazel:
```bash
bazel build //:std_slop
```




## Skill Hotwords
You can temporarily activate a skill for a single turn without permanently changing your session configuration using the `hey` hotword:

```bash
hey <skill_name> <your query>
```

> [!IMPORTANT]
> **Limitation in Sub-Queries**: The `hey` hotword detection does not work for sub-queries (e.g., within `llm_query` or `llm_query_async` calls). Sub-queries operate on a transient, in-memory database that only contains default system skills. Any non-default or custom skills registered in the main database will not be found by the `hey` detection logic inside a sub-query.

For example:
- `hey code_reviewer what do you think of this PR?`
- `hey sql_expert show me the last 5 logs from the messages table`

The system will:
1. Temporarily activate the requested skill.
2. Increment the skill's activation count for analytics.
3. Process your query.
4. Restore your previous active skills immediately after the response is generated.

Matching is case-insensitive, so `hey Code_Reviewer` and `hey code_reviewer` both work.

## Configuration
`std::slop` supports three levels of configuration, in order of precedence:
1.  **Command-line Flags** (e.g., `--model=gpt-4o`)
2.  **Configuration File** (`~/.config/slop/config.ini`)
3.  **Environment Variables** (e.g., `SLOP_DEBUG_HTTP`)

#

## Configuration File
By default, the application looks for a configuration file at `~/.config/slop/config.ini`. You can override this path using the `--config` flag:
```bash
bazel run //:std_slop -- --config=/path/to/my_config.ini
```

The file uses a standard INI format. All settings should be under the `[slop]` section. Keys in the INI file correspond directly to the command-line flags (replace hyphens with underscores).

Example `config.ini`:
```ini
[slop]
model = gemini-2.0-flash-exp
google_api_key = AIza...
```

See [example_config.ini](example_config.ini) for a template with all supported options.

### Environment Variables
For debugging purposes, you can use the following environment variable:

- `SLOP_DEBUG_HTTP=1`: Enable full verbose logging of all HTTP traffic (headers & bodies).

#### Recommended Settings for OpenRouter:
When using newer models via OpenRouter, it is highly recommended to use the `strip_reasoning = true` in your `config.ini` or the `--strip_reasoning` flag. This improves response focus and can reduce latency by preventing the reasoning chain from being included in the final output.

```bash
bazel run //:std_slop -- --strip_reasoning
```

## Running
Start a session by running the executable via Bazel. You can provide a session name to resume or categorize your work:
```bash
bazel run //:std_slop -- [session_name]
```
If no session name is provided, it defaults to `default_session`.

### Batch Mode (Prompt Mode)
For quick tasks or automation, you can run a single prompt in "Batch Mode" using the `--prompt` flag. In this mode, `std::slop` will process the prompt, execute any necessary tools, display the final response, and then exit immediately. This mode also supports `--session` to pick the session to work under, and `--model` to select the model from one of the models available at the endpoint.
`/commands` are supported as well.

```bash
bazel run //:std_slop -- --prompt "Summarize the files in the current directory"
```

You can combine this with `--session` to run a prompt within a specific persistent context:

```bash
bazel run //:std_slop -- --session "my_project" --prompt "What was the last thing we decided on the architecture?"
```

## Core Concepts

- **Session**: An isolated conversation history with its own settings and token usage tracking.
- **Group (GID)**: Every interaction (user prompt + assistant response + tool executions) is grouped under a unique `group_id`. This allows for atomic operations like `/undo`.
- **Context**: The window of past messages sent to the LLM. It can be a rolling window of the last `N` interactions or the full history.
- **Model Switching**: You can switch models (e.g., from Gemini to OpenAI) mid-session using the `/model` command. While conversational text is preserved across models, tool calls and results are isolated by provider (e.g., Gemini vs. OpenAI) to ensure reliable parsing and execution. Switching providers will hide previous tool interactions from the new model's immediate context.
- **State**: The persistent "Long-term RAM" for each session.
- **Scratchpad**: A persistent markdown workspace for evolving plans and task tracking. It is the agent's primary source of truth for task progress.
- **Skills**: Persona patches that inject specific instructions into the system prompt. These can be manually activated or automatically orchestrated by the agent.
- **Tools**: Executable functions (grep, file read, write_file, etc.) that the LLM can call.
- **Historical Retrieval**: The agent's ability to query its own database to find old context that has fallen out of the rolling window.

## Orchestration & Lua Integration

`std::slop` makes the LLM do everything with Lua.This is particularly useful for parallel operations, complex logic, or tasks that require multiple tool calls in a single turn.

### The `run_lua` Tool

The `run_lua` tool allows the agent to execute orchestrated scripts. It has access to:
- **`tools`**: A table containing all standard tools.
- **`history`**: Session message history for programmatic context extraction.
- **`llm_query`**: Synchronous and asynchronous helpers for isolated LLM sub-tasks.
- **Async Execution**: `tools.execute_bash_async` and `llm_query_async` allow for parallel execution (e.g., running multiple tests at once).

For more details, see the **[Lua Integration Documentation](lua_integration.md)**.

## Slash Commands

### Session Management
- `/exit` or `/quit`: Close the session.
- `/session list`: List all existing sessions.
- `/session switch <name>`: Switch to or create a new session named `<name>`. If the session does not exist, it will be created after the first call to the LLM.
- `/session remove <name>`: Delete a session and all its associated data (history, usage, state).
- `/session clear`: Wipe all messages and state for the *current* session, effectively starting fresh while keeping the same session ID.
- `/session clone <name>`: Clone the current session into a new session named `<name>`. This creates a complete copy of the history, scratchpad, and usage stats.
- `/session scratchpad read`: Display the current content of the session's scratchpad.
- `/session scratchpad edit`: Open the session's scratchpad in your system `$EDITOR`.

### Message Operations
- `/message list [N]` (Alias: `/messages list`): List the prompts of the last `N` message groups, including token usage for assistant responses.
- `/message show <GID>` (Alias: `/messages show`): View the full content (including tool calls/responses and token usage) of a specific group.
- `/message remove <GID>` (Alias: `/messages remove`): Hard delete a specific message group from history.
- `/undo`: Delete the very last interaction group and rebuild the session context.
- `/edit`: Open your last input in your system `$EDITOR` (e.g., vim, nano) and resend it after saving.

### Context Control
- `/context show`: Show current context settings and the fully assembled prompt that would be sent to the LLM. The output is human-readable and will automatically open in your `$EDITOR` if it exceeds terminal height.
- `/context window <N>`: Limit the context to the last `N` interaction groups. Set to `0` for infinite history.
- `/context rebuild`: Force a rebuild of the in-memory session state from the SQL message history. Useful if the database was modified externally.


### Knowledge Management (Memos)
- `/memo list`: List all saved memos and their tags.
- `/memo show <id>`: View the full content of a specific memo.
- `/memo search <query>`: Search for memos containing a specific keyword or tag.
- `/memo remove <id>`: Permanently delete a memo.
- `/memo add`: Manually add a new memo via your system `$EDITOR`. The content is parsed as Markdown.
- `/memo edit <id>`: Edit an existing memo in your system `$EDITOR`. Skills and memos are now edited using Markdown format with automatic parsing of headers and content.

Memos are long-term, cross-session pieces of knowledge. While you can manage them via these commands, the LLM is also equipped with `save_memo` and `retrieve_memos` tools to autonomously manage knowledge for you.

### Skills & Orchestration
- `/skill list`: List all available and active skills.
- `/skill activate <name|id>`: Enable a skill for the current session.
- `/skill deactivate <name|id>`: Disable a skill for the current session.
- `/skill add`: Interactive prompt to create a new skill.
- `/skill edit <name|id>`: Modify an existing skill in your system `$EDITOR`. Skills are edited using Markdown format with YAML frontmatter for metadata (name, description) and the body for the system prompt patch.
- `/skill delete <name|id>`: Permanently remove a skill from the database.

#### Dynamic Skill Orchestration
`std::slop` is designed to be proactive. If you ask the agent to perform a task that requires a specialized persona (like "Plan a feature" or "Review this code"), it will:
1.  **Search** the `skills` table for a matching persona (e.g., `planner`, `code_reviewer`).
2.  **Self-Activate**: It will read the `system_prompt_patch` for that skill and adopt its instructions for the current task.
3.  **Self-Deactivate**: Once the specific task is complete, it will automatically return to its core "cli agent" persona.

If a task is expected to span many turns, the agent may recommend that you permanently activate the skill using `/skill activate`.

#### Skill Composition
`std::slop` supports the activation of multiple skills simultaneously. For example, you might have the `planner` skill active alongside a project-specific architectural skill.
- **Non-Conflicting**: Skills should generally be additive.
- **Conflicts**: If two skills provide conflicting instructions (e.g., one requires "No comments" and another requires "Doxygen comments"), the agent will attempt to reconcile them or ask for clarification.

#### Expanding the Skill Library
`std::slop` is designed to grow with your project. Both you and the agent are encouraged to add new skills to the `skills` table when a repeatable persona or set of constraints is identified. 
- **Agent-led**: The agent may suggest creating a new skill if it detects a recurring specialized workflow.
- **User-led**: Use `/skill add` or `query_db` to insert new entries. This is particularly useful for project-specific constraints (e.g., "Must use library X", "Adhere to architectural pattern Y").

#### Example Skills (these are provided as defaults)

**planner**
- **Description**: Technical Lead specialized in architectural decomposition and iterative feature delivery.
- **System Prompt Patch**:
```text
You only plan. You _do_ _not_ implement anything, and do not write or modify any files. You give me ideas to plan ONLY!
```

**dba**
- **Description**: Database Administrator specializing in SQLite schema design, optimization, and data integrity.
- **System Prompt Patch**:
```text
As a DBA, you are the manager of the project's data. You focus on schema design, precise query construction, and maintaining data integrity. When interacting with the database: 1. Always verify schema before operations. 2. Use transactions for complex updates. 3. Provide clear explanations for schema changes. 4. Optimize for performance while ensuring clarity.
```

**c++_expert**
- **Description**: Enforces strict adherence to project C++ constraints: C++17, Google Style, no exceptions, RAII/unique_ptr, absl::Status.
- **System Prompt Patch**:
```text
You are a C++ Expert specialized in the std::slop codebase.
You MUST adhere to these constraints in every code change:
- Language: C++17.
- Style: Google C++ Style Guide.
- Exceptions: Strictly disabled (-fno-exceptions). Never use try, catch, or throw.
- Memory: Use RAII and std::unique_ptr exclusively. Avoid raw new/delete. Use stack allocation where possible.
- Error Handling: Use absl::Status and absl::StatusOr for all fallible operations.
- Threading: Avoid threading and async primitives. If necessary, use absl based primitives with std::thread and provide tsan tests.
- Design: Prefer readable code over complex template metaprogramming or deep inheritance.
You ALWAYS run all the tests and ensure the affected targets compiles correctly.
```

**code_reviewer**
- **Description**: Multilingual code reviewer enforcing language-specific standards (Google C++, PEP8, etc.) and project conventions.
- **System Prompt Patch**:
```text
You are a strict code reviewer. Your goal is to review code changes against industry-standard style guides and project conventions. 
Standards to follow:
- C++: Google C++ Style Guide.
- Python: PEP 8.
- Others: Appropriate de-facto industry standards (e.g., Effective Java, Airbnb JS Style Guide).
You do NOT implement changes. You ONLY provide an annotated set of required changes or comments. Only after explicit user approval can you proceed with addressing the issues identified. Focus on style, safety, and readability. For new files, use `git add --intent-to-add` before `git diff`. Always list the files reviewed in your summary.
```

### Reviewing Changes
`std::slop` provides two primary ways to review code changes before they are finalized. See also the [mail model](mail_model.md). The Mail Model is the recommended  way to use std::slop to code your project without losing your mind.

#### 1. Automated Review (via `code_reviewer` skill)
This flow uses the LLM's knowledge of industry standards (like the Google C++ Style Guide) to automatically identify issues.
- **Activate**: Run `/skill activate code_reviewer`.
- **Trigger**: Simply ask the agent: "review the changes".
- **Execution**: The agent will run `git diff` (including new files) and provide an annotated summary of required changes.
- **Safety**: The `code_reviewer` persona is instructed **not** to implement changes without explicit user approval after the review is presented.

#### 2. Manual Review (via `/review` command)
This flow allows the human user to provide precise, line-by-line or general instructions on the current changeset.
- **Trigger**: Run the `/review` command.
- **Editor**: Your system `$EDITOR` (e.g., `vim`, `nano`) will open with a diff of all pending changes (including intent-to-add for new files).
- **Providing Feedback**: Add your comments directly into the editor on new lines starting with `R:`. For example:
  ```text
  R: This variable name should be more descriptive.
  R: Please add a comment explaining this complex logic.
  ```
- **Processing**: When you save and exit the editor, `std::slop` sends your `R:` comments along with the diff back to the LLM. The agent will then attempt to address each of your specific points.

#### 3. Assistant Feedback (via `/feedback` command)
Similar to `/review`, this allows you to provide line-by-line feedback on the **last assistant message**. This is useful for correcting reasoning errors or providing specific feedback on generated text/code without waiting for a commit.
- **Trigger**: Run the `/feedback` command.
- **Editor**: Opens the last assistant response with line numbers.
- **Providing Feedback**: Add your comments on new lines starting with `R:`.
- **Processing**: If comments are found, they are sent back to the LLM to address in its next turn. If no `R:` comments are added, the command is ignored.

## Rate Limit Handling

std::slop implements intelligent retry logic to gracefully handle API rate limits (HTTP 429):

| Feature | Behavior |
|---------|----------|
| **Backoff Strategy** | Exponential backoff with jitter |
| **Default Delay** | 5 seconds |
| **Jitter Range** | ±10% random variance (±450ms at default) |
| **Max Retries** | 6 attempts |
| **Terminal Detection** | Skips retries for daily quota errors ("QUOTA_EXHAUSTED") |

### Manual Control
- `/throttle N` - Set delay between automatic requests in seconds (default: 1)
- `/throttle 0` - Disable automatic delays (not recommended)

## Troubleshooting & Debugging

### HTTP Logging
If you encounter issues with API calls (e.g., unexpected errors from the LLM provider), you can enable full HTTP message logging by setting the `SLOP_DEBUG_HTTP` environment variable:

```bash
export SLOP_DEBUG_HTTP=1
bazel run //:std_slop -- --log slop_verbose.log
```

When enabled, `std::slop` will log all CURL activity to the standard log, including request/response headers and full bodies. This is useful for identifying issues with payload formats or rate-limiting responses.

### Prompt Inspection (Debugging)
To see the exact JSON payload being sent to the LLM (after context assembly and strategy-specific formatting), use the `SLOP_TOOL_DEBUG` environment variable:

```bash
export SLOP_TOOL_DEBUG=1
bazel run //:std_slop
```

When enabled, the final assembled prompt will be logged via `absl::LOG(INFO)`. This is distinct from HTTP logging as it shows the internal logical representation before it is transmitted.

### Other Commands
- `/models`: List all models available for your current provider.
- `/model <name>`: Switch to a different LLM model.
- `/throttle [N]`: Set a pause (in seconds) between automatic agent interactions to prevent rate limiting or to allow for human review.
- `/exec <command>`: Run a shell command and view its output in a pager.
- `/usage` or `/stats`: View total token usage for the current session.
- `/schema`: View the internal database schema for the `messages` ledger.

## Concurrency & Control

### Parallel Tool Execution
`std::slop` executes tool calls in parallel by spawning a new thread for each tool call. This allows the agent to perform multiple searches or file reads simultaneously, significantly reducing turn-around time for complex tasks. 

All concurrent tasks are managed via the Lua Control Plane and can be cancelled individually or as a group.

### Mail Mode (Patch-Based Workflow)
For complex features that require multiple iterations and commit history, use **Mail Mode**.

1. **Activate**: `/mode mail` (Requires a Git repository).
2. **Indicator**: The modeline will show `std::slop <📬 MAIL_MODEL | ...>` (in green) and the `patcher` skill will be active. In standard mode, it shows `std::slop <🤖 STANDARD | ...>` (in cyan).
3. **Tooling**: Use `git_branch_staging`, `git_commit_patch`, `git_format_patch_series`, `git_verify_series`, `git_reroll_patch`, and `git_finalize_series`.
4. **Review**: Use `/review mail [index]` to review specific patches. Use `R:` comments in the editor for feedback.
5. **Approval**: Run `/review mail approve` to sign the current patchset. This is **required** before calling `git_finalize_series`. Approval is non-sticky; any new commit or reroll will invalidate it.

For more details, see [mail_model.md](mail_model.md).

### Interrupting Active Tasks
If a tool is taking too long (e.g., a massive `grep` or a complex build), or if you realize the agent is going in the wrong direction, you can interrupt the current turn.

- **Press `[Esc]`**: Cancels all currently executing tools. 
  - Shell processes are terminated using process groups to ensure no "zombie" processes are left behind.
  - Network requests are immediately aborted.
  - The results are returned to the LLM with a `[Cancelled]` status, allowing it to recover or ask for clarification.
- **Press `[Ctrl+C]`**: Triggers a graceful shutdown of the entire application, ensuring the database is committed and the terminal state is restored.
