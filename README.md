# std::slop
  

[![CI/CD - Multi-Platform Build & Release](https://github.com/hsaliak/std_slop/actions/workflows/ci_cd.yml/badge.svg)](https://github.com/hsaliak/std_slop/actions/workflows/ci_cd.yml)

[![CodeQL Advanced](https://github.com/hsaliak/std_slop/actions/workflows/codeql.yml/badge.svg)](https://github.com/hsaliak/std_slop/actions/workflows/codeql.yml)

![std::slop](docs/slop.png)

`std::slop` is a persistent, SQLite-driven C++ CLI coding agent. It exposes direct, schema-validated tools for repository inspection, exact edits, unified patches, shell validation, database access, scratchpad state, and mail-mode git workflows.

Direct tools keep ordinary operations explicit and individually reviewable. Mail workflow tools enforce staging, review, approval, and finalization protections server-side.

## ✨ Key Features

- **🧰 Direct coding tools**: Inspect, edit, patch, validate, and query through independent schema-validated calls.
- **🎭 Personas & Skills**: Define global agent instructions via `AGENTS.md` and extend capabilities using modular, on-demand `SKILL.md` files.
- **🧭 Workflow guidance**: Skills help agents perform bounded repository surveys, focused reviews, and validation with direct tools.
- **📖 Ledger-Driven**: All interactions and tool calls are stored in SQLite for persistence and auditability. 
- **📝 Session Scratchpad**: Maintain a per-session planning buffer with `/scratchpad edit`, `/scratchpad save`, and `read_scratchpad`/`write_scratchpad` tools.
- **🎛️ Context Control**: SQL-backed, per-session accordion history preserves an append-only prompt prefix between resets, so sessions can grow independently while retaining cache-friendly context.
- **🪗 Accordion context**: Use `/context <retain_groups> [watermark_tokens]` to grow a cache-friendly prompt prefix, then reset to complete recent groups after the latest actual prompt usage reaches the watermark (defaults: `2`, `350000`). Tool results remain full fidelity up to their configured per-result limit.
- **📬 Mail workflows**: Use [docs/mail_mode.md](docs/mail_mode.md) for the manual patch-based workflow or [docs/mail-loop/README.md](docs/mail-loop/README.md) for the automated mail-loop orchestrator.
- **🤖 Multi-Model**: Supports OpenAI-compatible APIs (OpenRouter, etc.) and OpenAI Responses API (with chatgpt plus/pro oauth).
- **📣 Hotwords**: Quick, single-turn skill activation using `hey <skill> <query>` syntax. Eg: "hey code_reviewer review these patches".

## 🚀 Quick Start

### Download
The project ships Linux x86-64 and macOS binaries every [release](https://github.com/hsaliak/std_slop/releases). You can directly use them.

### 📋 Prerequisites
- C++17 compiler (Clang/GCC)
- [Bazel](https://bazel.build/install) (Bazelisk recommended)
- **Git**: Targets must be valid git repositories. Usually, a git add and an initial commit is sufficient to trigger all the git enabled features.

### 🛠️ Build and Install
```bash
# Build the binary
bazel build //:std_slop

# Optional: Add to your PATH
cp ./bazel-bin/app/std_slop /usr/local/bin/
```

### ⌨️ Usage
`std::slop` works best when it can track a specific project. Initialize a git repository and run it from the root:
```bash
mkdir my-project && cd my-project
git init
std_slop
```

For quick one-off tasks, you can use **Batch Mode**:
```bash
std_slop --prompt "Refactor main.cpp to remove all unused includes"
```
Batch mode accepts exactly one instruction source, and optional piped stdin is prepended as context:
```bash
std_slop --prompt-file task.md
ls *.cc | std_slop --prompt "sort these files in alphabetical order"
cat errors.log | std_slop --prompt-file diagnose.md
```
Use exactly one instruction source: `--prompt` or `--prompt-file`. Empty instructions are rejected. Empty or whitespace-only piped stdin is ignored.

For scripts, batch mode can emit one run-metadata JSON object to stdout:
```bash
std_slop --prompt-file task.md --output json | jq -r .assistant_message
```
The JSON object contains `ok`, `session`, `model`, `active_skills`, `assistant_message`, `structured_output`, `error`, and `duration_ms`.

You can combine prompt mode with piped context, an explicit model, a throwaway session, and a persistent prompt database when a one-off task needs tool calls or auditability:
```bash
cat errors.log | std_slop \
  --prompt-file diagnose.md \
  --model gpt-5.4-mini:high \
  --session ci-diagnose \
  --prompt-db /tmp/slop-ci-diagnose.db
```

Batch mode can also require the model's final answer to match a validated JSON Schema object. Use `--format` for an inline schema or `--format_file` for a schema file; exactly one is allowed, and it cannot be combined with `--output json` because stdout is the raw structured result:
```bash
std_slop --prompt "Extract the name" \
  --format '{"type":"object","properties":{"name":{"type":"string"}},"required":["name"],"additionalProperties":false}'
```

For larger schemas, store the schema in a file and pass it with `--format_file`:
```bash
cat > /tmp/person.schema.json <<'JSON'
{
  "type": "object",
  "properties": {
    "name": { "type": "string" },
    "age": { "type": "integer" },
    "skills": {
      "type": "array",
      "items": { "type": "string" }
    }
  },
  "required": ["name", "age"],
  "additionalProperties": false
}
JSON

std_slop \
  --prompt "Extract the person's name, age, and skills from: Ada is 37 and knows C++ and SQL." \
  --format_file /tmp/person.schema.json
```

All pieces can be used together: stdin becomes context, `--prompt-file` supplies the instruction, `--format_file` enforces the final JSON shape, and the raw validated JSON object is the only stdout payload:
```bash
cat incident.log | std_slop \
  --prompt-file summarize_incident.md \
  --format_file incident_summary.schema.json \
  --model gpt-5.4-mini:high \
  --session incident-2026-07-19 \
  --prompt-db /tmp/slop-incident.db
```
Structured output supports this bounded schema subset: root `type: "object"`, nested object/array/string/number/integer/boolean/null types, `properties`, `required`, boolean `additionalProperties`, array `items`, and non-empty `enum` arrays.

Batch mode also takes in `--model` which is useful to specify the model to use and `--session` which is useful to indicate the session the prompt should be executed under. Batch mode works off an in memory sqlite db. If you want the db persisted you can point it to a DB with the `--prompt-db` argument.
`/commands` are also supported. 


Read the [Walkthrough](docs/WALKTHROUGH.md) first for the recommended getting-started flow, authentication setup paths, `config.ini` setup, docs-folder navigation, and `llm_query` subquery/persona configuration. Then use [docs/README.md](docs/README.md) as the docs index for deeper reference material.

### Authentication Quick Notes
- OpenAI-compatible: set `OPENAI_API_KEY` or put it in `~/.config/slop/config.ini`
- OpenAI-compatible API key: set `OPENAI_API_KEY`, optionally combine with `--openai_base_url`, or put both in `config.ini`
- OpenAI OAuth (Responses API): run `std_slop --fetch_openai_oauth_token` or `std_slop --fetch_openai_oauth_device_token`, then start with `--openai_oauth`

### ⚙️ Configuration
You can configure `std::slop` using environment variables or a configuration file.

#### Configuration File
The agent looks for a configuration file at `~/.config/slop/config.ini`. You can also specify a custom path using the `--config` flag.
It is STRONGLY RECOMMENDED that slop.db lies in a central directory or outside the codebase. It generates 2 other artifact files, at least ensure that
your .gitignore contains this. The context ledger is completely stored in the database, and it can inadvertently capture information from your environment if you are not careful. Eg Environment Variables.

For a getting-started walkthrough that covers config methods end-to-end, see [docs/WALKTHROUGH.md](docs/WALKTHROUGH.md).

```ini
[slop]
model = your-model-name
# OR
openai_api_key = sk-...
openai_base_url = https://api.openai.com/v1

# openai_oauth = true    # optional: use OpenAI OAuth token + Responses API
# openai_oauth_token_path = /custom/path/chatgpt_plus_token.json
```

See [docs/example_config.ini](docs/example_config.ini) for a full list of options.

#### Configure LLM sub-agents (specialized `llm_query` tools)
You can define config-based `llm_query` specializations as first-class tools. This
is useful for role-focused delegation (for example: code review, repo exploration)
without rewriting prompts each time.

Add one INI section per specialization using the `llm_tool_` prefix:

```ini
[llm_tool_code_review_llm]
system_prompt_patch = You are a strict code reviewer focused on correctness and regressions.
session_id = code_review
skill = code_reviewer
context_window = 8
```

After startup, call the specialized tool directly by name (for example
`llm_tool_code_review_llm`) with a `query` argument.

For a complete multi-specialization example, see
[docs/example_subqueries.ini](docs/example_subqueries.ini).
Detailed behavior and policy constraints are documented in
[docs/impl/subqueries.md](docs/impl/subqueries.md).

#### Environment Variables
- `SLOP_DEBUG_HTTP=1`: Enable full verbose logging of all HTTP traffic (headers & bodies).


## 💻 Code

- C++ Standard: C++17.
- Style: Google C++ Style Guide.
- Exceptions: Disabled (-fno-exceptions).
- Memory: RAII and std::unique_ptr exclusively.
- Error Handling: absl::Status and absl::StatusOr.
- Asan and Tsan clean at all times.

## 📚 Documentation

- **[Personas & Skills](docs/CONTEXT.md)**: Understanding global context injection and modular skills.
- **[Documentation Guide](docs/README.md)**: Entry point and reading order for the documentation set.
- **[Architecture & Schema](docs/SCHEMA.md)**: Understanding the database-driven engine.
- **[Sessions](docs/SESSIONS.md)**: How context isolation and management work.
- **[Context Management](docs/CONTEXT_MANAGEMENT.md)**: The history and strategy for managing model memory.
- **[Walkthrough](docs/WALKTHROUGH.md)**: A step-by-step example of using the agent.
- **[Subquery Implementation Notes](docs/impl/subqueries.md)**: Design and policy notes for INI-configured `llm_query` specializations.
- **[Fuzzing](docs/fuzzing.md)**: FuzzTest targets, invariants, and how to run/extend the fuzz suite.
- **[Contributing](docs/CONTRIBUTING.md)**: Code style, formatting, and linting guidelines.

## 🏗️ Architecture & Codebase Layout

### `core/` - The Engine
The core logic is divided into modules:

- **`database.h`**: Manages the SQLite-backed ledger. Handles persistence for messages, memos, tools, and skills.
- **`tool_dispatcher.h`**: Implements a thread-safe execution engine. It dispatches multiple tool calls concurrently while ensuring results are returned in the proper order for the LLM.
- **`cancellation.h`**: Provides a mechanism for interrupting tasks. It supports registering callbacks to kill shell processes or abort HTTP requests.
- **`orchestrator.h`**: high-level interface for model interaction. The Responses API orchestrator manages history windowing and response parsing.
- **`shell_util.h`**: Executes shell commands in a separate process group, with support for live output polling and termination on cancellation.
- **`http_client.h`**: A minimalist, cancellation-aware HTTP client used for all model API calls.

### Interface & Display
- **`interface/`**: Implements the terminal UI. The UI is minimal but clean, uses readline for user input, color codes and ASCII Codes.
- **`markdown/`**: Uses `tree-sitter-markdown` to provide syntax highlighting (C++, Python, Go, JS, Rust, Bash) and structured rendering for agent responses. This is a stand alone Markdown  parser / renderer library in C++.
- **`app/main.cpp`**: The primary event loop. Coordinates between the Orchestrator, ToolDispatcher, and UI.






