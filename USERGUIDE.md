# std::slop User Guide

## Overview
`std::slop` is a high-performance LLM CLI built for developers who want a SQL-backed, persistent conversation history with built-in tools for codebase exploration and context management.

## Installation
Build using Bazel:
```bash
bazel build //:std_slop
```

## Setup
You need an API key for either Google Gemini or an OpenAI compatible endpoint. By default, `std::slop` uses OpenRouter for OpenAI-compatible models.

### For Gemini:
```bash
export GOOGLE_API_KEY="your_api_key"
```
Or use Google OAuth (recommended):
```bash
bazel run //:std_slop
```
If no API keys are found, the CLI defaults to Google OAuth. It automatically discovers your project ID using the authoritative `loadCodeAssist` endpoint.

To authenticate, run the provided script:
```bash
./slop_auth.sh
```
The script will provide a URL for you to visit. After authorizing, paste the **full redirect URL** back into the script, and it will automatically extract the tokens and save them to `~/.config/slop/token.json`.


### For OpenAI/OpenRouter:
We default `OPENAI_BASE_URL` to `https://openrouter.ai/api/v1`. 
To use OpenAI proper or another provider, override it:
```bash
export OPENAI_BASE_URL="https://api.openai.com/v1"
```

Set your API key:
```bash
export OPENAI_API_KEY="your_api_key"
```

### Recommended Settings for OpenRouter:
When using newer models via OpenRouter, it is highly recommended to use the `--strip_reasoning` flag. This improves response focus and can reduce latency by preventing the reasoning chain from being included in the final output.

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
- **Scratchpad**: A flexible, persistent markdown workspace for evolving plans and task tracking. It is the agent's primary source of truth for task progress.
- **Skills**: Persona patches that inject specific instructions into the system prompt. These can be manually activated or automatically orchestrated by the agent.
- **Tools**: Executable functions (grep, file read, write_file, etc.) that the LLM can call.
- **Historical Retrieval**: The agent's ability to query its own database to find old context that has fallen out of the rolling window.

## Search & Discovery

The agent provides powerful search tools designed for large codebases.

### `git_grep_tool`
- **Boolean Queries**: Supports `--and`, `--or`, `--not`, and grouping with `(`, `)`.
  - Example patterns: `["(", "pattern1", "--and", "pattern2", ")", "--or", "pattern3"]`
- **Multiple Pathspecs**: Search across multiple directories or file patterns at once.
- **Rich Context**: Use `function_context: true` to see the full body of matching functions.
- **Smart Truncation**: Results are capped at 500 lines to balance detail with context usage.

### `list_directory`
- Provides recursive directory listings with optional depth control.
- Defaults to git-tracked files for faster exploration in large repositories.

## User Interface

`std::slop` features an enhanced CLI UI designed for readability:
- **Iconography**: Every message is prefixed with a semantic icon:
  - 🧠 **Thought**: AI reasoning or planning.
  - 🛠️ **Tool**: Tool execution initiation.
  - ✅ **Success**: Successful completion of a tool or command.
  - ❌ **Error**: Failure or invalid operation.
  - ⚠️ **Warning**: Cautionary notices.
  - ℹ️ **Info**: Neutral system messages.
  - 📥 **Input**: Data being received or read.
  - 📤 **Output**: Data being written or sent.
  - 📝 **Memo**: Interaction with the long-term knowledge base.
  - 🎓 **Skill**: Activation or deactivation of specialized personas.
  - 🕒 **Session**: Timeline and state management.
- **Colors**: Tool headers are displayed in grey. Assistant messages are white. Indentation is used for hierarchy.
- **Dynamic Modeline**: The prompt updates in real-time to reflect the current state: `std::slop<W:window_size, M:model, P:persona, S:session_id, T:throttle>`.
- **Syntax Highlighting**: Fenced code blocks in assistant responses are automatically highlighted using Tree-sitter. Currently supported languages include:
  - C / C++
  - Python
  - JavaScript
  - Go
  - Rust
  - Bash / Shell
- **Markdown Tables**: Long tables are intelligently handled. If a table is wider than the terminal, `std::slop` will shrink the widest columns and wrap the text within them into multiple lines. This ensures the table remains readable and fits within your terminal window without losing information.
- **Truncation**: Tool calls and their results are automatically truncated to 60 columns to prevent terminal clutter. Note that for the LLM's context, tool results are preserved at high fidelity (5000 chars) for the active turn but aggressively compressed (500 chars) for historical turns to save tokens.

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
- **Description**: Strategic Tech Lead specialized in architectural decomposition and iterative feature delivery.
- **System Prompt Patch**:
```text
You only plan. You _do_ _not_ implement anything, and do not write or modify any files. You give me ideas to plan ONLY!
```

**dba**
- **Description**: Database Administrator specializing in SQLite schema design, optimization, and data integrity.
- **System Prompt Patch**:
```text
As a DBA, you are the steward of the project's data. You focus on efficient schema design, precise query construction, and maintaining data integrity. When interacting with the database: 1. Always verify schema before operations. 2. Use transactions for complex updates. 3. Provide clear explanations for schema changes. 4. Optimize for performance while ensuring clarity.
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
- Design: Prefer simple, readable code over complex template metaprogramming or deep inheritance.
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

### Automation Workflow
The Planner can be used to break a large feature into small, atomic tasks which can then be implemented iteratively. These plans are stored and evolved in the **Scratchpad**, which is **automatically injected** into every turn, allowing the LLM to track its progress even when history is truncated.

1.  **Decompose**: Use the `planner` skill to break a large feature into steps.
2.  **Initialize Scratchpad**: The `planner` will automatically use the `manage_scratchpad` tool to save the initial roadmap.
3.  **Execute & Iterate**: As the agent works, it will autonomously update the scratchpad checklist as steps are completed. You no longer need to explicitly ask the model to "read the scratchpad"—it is always visible.

### Reviewing Changes
`std::slop` provides two primary ways to review code changes before they are finalized.

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
`std::slop` executes tool calls in parallel using a thread pool. By default, it uses up to 4 concurrent threads. This allows the agent to perform multiple searches or file reads simultaneously, significantly reducing turn-around time for complex tasks. 

You can configure the parallelism level using the `--max_parallel_tools` flag:
```bash
bazel run //:std_slop -- --max_parallel_tools=8
```

### Mail Mode (Patch-Based Workflow)
For complex features that require multiple iterations and clean commit history, use **Mail Mode**.

1. **Activate**: `/mode mail` (Requires a Git repository).
2. **Indicator**: The modeline will show `std::slop <📬 MAIL_MODEL | ...>` (in green) and the `patcher` skill will be active. In standard mode, it shows `std::slop <🤖 STANDARD | ...>` (in cyan).
3. **Tooling**: Use `git_branch_staging`, `git_commit_patch`, `git_format_patch_series`, `git_verify_series`, `git_reroll_patch`, and `git_finalize_series`.
4. **Review**: Use `/review mail [index]` to review specific patches. Use `R:` comments in the editor for feedback.

For more details, see [docs/mail_model.md](docs/mail_model.md).

### Interrupting Active Tasks
If a tool is taking too long (e.g., a massive `grep` or a complex build), or if you realize the agent is going in the wrong direction, you can interrupt the current turn.

- **Press `[Esc]`**: Cancels all currently executing tools. 
  - Shell processes are terminated using process groups to ensure no "zombie" processes are left behind.
  - Network requests are immediately aborted.
  - The results are returned to the LLM with a `[Cancelled]` status, allowing it to recover or ask for clarification.
- **Press `[Ctrl+C]`**: Triggers a graceful shutdown of the entire application, ensuring the database is committed and the terminal state is restored.
