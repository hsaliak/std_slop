# std::slop
  

[![CI/CD - Multi-Platform Build & Release](https://github.com/hsaliak/std_slop/actions/workflows/ci_cd.yml/badge.svg)](https://github.com/hsaliak/std_slop/actions/workflows/ci_cd.yml)

[![CodeQL Advanced](https://github.com/hsaliak/std_slop/actions/workflows/codeql.yml/badge.svg)](https://github.com/hsaliak/std_slop/actions/workflows/codeql.yml)

![std::slop logo](docs/slop.png)

`std::slop` is a persistent, SQLite-driven C++ CLI coding agent. 

It exposes direct, schema-validated tools for repository inspection, exact edits, unified patches, shell validation, database access, scratchpad state, and mail-mode git workflows. Direct tools keep ordinary operations explicit and individually reviewable. Mail workflow tools enforce staging, review, approval, and finalization protections server-side. Mail Mode is ideal when manual code review still happens: it breaks work into small, discrete, reviewable patch sets that preserve clear rationale and support reliable git bisection.

## Key Features

- **🎭 Personas & Skills**: Define global agent instructions via `AGENTS.md` and extend capabilities using modular, on-demand `SKILL.md` files.
- **📖 Ledger-Driven**: All interactions and tool calls are stored in SQLite for persistence and auditability. 
- **📝 Session Scratchpad**: Maintain a per-session planning buffer with `/scratchpad edit`, `/scratchpad save`, and `read_scratchpad`/`write_scratchpad` tools.
- **🎛️ Context Control**: SQL-backed, per-session accordion history preserves an append-only prompt prefix between resets, so sessions can grow independently while retaining cache-friendly context.
- **🪗 Accordion context**: Use `/context <retain_groups> [watermark_tokens]` to grow a cache-friendly prompt prefix, then reset to complete recent groups after the latest actual prompt usage reaches the watermark (defaults: `2`, `350000`). Tool results remain full fidelity up to their configured per-result limit.
- **Mail workflows**: Use [docs/mail_mode.md](docs/mail_mode.md) for a review-first patch workflow. Careful code review remains the authority while each small, discrete patch stays independently reviewable, verifiable, and suitable for git bisection.
- **Models**: Supports OpenAI-compatible Responses endpoints and ChatGPT Plus/Pro OAuth. Practically, routers such as OpenRouter make this sufficient for accessing a variety of models.
- **Hotwords**: Activate a skill for one turn with `hey <skill> <query>`.

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
std_slop --prompt_file task.md
ls *.cc | std_slop --prompt "sort these files in alphabetical order"
```
Use exactly one instruction source: `--prompt` or `--prompt_file`. Piped stdin is optional context. Batch mode uses an in-memory database unless `--prompt_db` is set.

For scripts, `--output=json` writes run metadata:
```bash
std_slop --prompt_file task.md --output=json | jq -r .assistant_message
```
The JSON object contains `ok`, `session`, `model`, `active_skills`, `assistant_message`, `structured_output`, `error`, and `duration_ms`.

Use `--format` or `--format_file` to require a JSON Schema-constrained final value. The raw validated value is the only stdout payload, so structured output cannot be combined with `--output=json`:
```bash
std_slop --prompt "Extract the name" \
  --format '{"type":"object","properties":{"name":{"type":"string"}},"required":["name"],"additionalProperties":false}'

cat incident.log | std_slop \
  --prompt_file summarize_incident.md \
  --format_file incident_summary.schema.json \
  --model gpt-5.4-mini:high \
  --session incident-2026-07-19 \
  --prompt_db /tmp/slop-incident.db
```
The supported schema subset is a root object plus nested object, array, string, number, integer, boolean, and null types; `properties`, `required`, boolean `additionalProperties`, `items`, and non-empty `enum` arrays.


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
- **[MCP API](docs/mcp-api.md)**: Reusable C++ MCP client library surface, bearer token support, and OAuth helper APIs.
- **[MCP User Guide](docs/mcp-slop-userguide.md)**: How `std_slop` registers, authenticates, discovers, and exposes MCP tools.
- **[Subquery Implementation Notes](docs/impl/subqueries.md)**: Design and policy notes for INI-configured `llm_query` specializations.
- **[Fuzzing](docs/fuzzing.md)**: FuzzTest targets, invariants, and how to run/extend the fuzz suite.
- **[Contributing](docs/CONTRIBUTING.md)**: Code style, formatting, and linting guidelines.

## 🏗️ Architecture & Codebase Layout

### `core/` - The Engine
The core logic is divided into modules:

- **`database.h`**: Manages the SQLite-backed ledger. Handles persistence for messages, tools, skills, sessions, and usage data.
- **`tool_dispatcher.h`**: Implements a thread-safe execution engine. It dispatches multiple tool calls concurrently while ensuring results are returned in the proper order for the LLM.
- **`cancellation.h`**: Provides a mechanism for interrupting tasks. It supports registering callbacks to kill shell processes or abort HTTP requests.
- **`orchestrator.h`**: high-level interface for model interaction. The Responses API orchestrator manages history windowing and response parsing.
- **`shell_util.h`**: Executes shell commands in a separate process group, with support for live output polling and termination on cancellation.
- **`http_client.h`**: A minimalist, cancellation-aware HTTP client used for all model API calls.

### Interface & Display
- **`interface/`**: Implements the terminal UI. The UI is minimal but clean, uses readline for user input, color codes and ASCII Codes.
- **`markdown/`**: Uses `tree-sitter-markdown` to provide syntax highlighting (C++, Python, Go, JS, Rust, Bash) and structured rendering for agent responses. This is a stand alone Markdown  parser / renderer library in C++.
- **`app/main.cpp`**: The primary event loop. Coordinates between the Orchestrator, ToolDispatcher, and UI.






