# std::slop
  

[![CI/CD - Multi-Platform Build & Release](https://github.com/hsaliak/std_slop/actions/workflows/ci_cd.yml/badge.svg)](https://github.com/hsaliak/std_slop/actions/workflows/ci_cd.yml)

[![CodeQL Advanced](https://github.com/hsaliak/std_slop/actions/workflows/codeql.yml/badge.svg)](https://github.com/hsaliak/std_slop/actions/workflows/codeql.yml)

![std::slop logo](docs/slop.png)

`std::slop` is a C++ monorepo for agentic tooling. It provides the `std_slop` interactive coding agent, the `sl` command-line tool, MCP client and server libraries with examples, and a Markdown parser and terminal renderer.

## Components

| Component | Purpose | Guide |
| --- | --- | --- |
| std::slop coding agent (`std_slop`) | Run interactive terminal sessions or batch prompts. | [Agent walkthrough](docs/WALKTHROUGH.md) |
| `sl` command-line tool | Run prompts and inspect agent state from scripts. | [CLI guide](docs/sl.md) |
| MCP client library and examples | Connect C++ applications to Streamable HTTP or modern local stdio MCP servers. | [Client API](docs/mcp-api.md) |
| MCP server library and example | Expose explicitly registered tools over stdio. | [Server API](docs/mcp-server.md) |
| Agent database and schema | Manage sessions, tools, model calls, and patch review state. | [Runtime data model](docs/SCHEMA.md) |
| Markdown library | Parse Markdown and render styled terminal output using Tree-sitter. | [Parser and renderer](markdown/README.md) |

## Agent entry points

Build both interfaces:

```sh
bazel build //:std_slop //:sl
```

After [model authentication and configuration](docs/WALKTHROUGH.md), start the terminal UI or run a scripted prompt:

```sh
bazel-bin/app/std_slop
bazel-bin/app/sl --prompt "Summarize the repository structure"
```

Use the [walkthrough](docs/WALKTHROUGH.md) for interactive sessions and the [sl guide](docs/sl.md) for scripts. Batch mode, configuration, and workflow details follow below.

## Library quick starts

Build the MCP client examples:

```sh
bazel build //mcp/client:list_tools_example //mcp/client:call_tool_example
```

See the [client guide](mcp/client/README.md) for HTTP authorization, modern stdio connections, and argv configuration.

Build and test the stdio echo server:

```sh
bazel build //mcp/server:echo_server
bazel test //mcp/server:echo_server_test
```

See the [server guide](docs/mcp-server.md) for requests, framing limits, and tool registration.

Build the Markdown parser and terminal renderer:

```sh
bazel build //markdown:parser //markdown:renderer
```

See the [Markdown guide](markdown/README.md) for parser and terminal renderer APIs.

## Component boundaries

- The Markdown library and echo server do not need a model, API key, or agent database. The echo integration test needs Python 3.
- MCP clients support Streamable HTTP and modern MCP `2026-07-28` over local stdio. The inbound server package supports stdio only. Agent runtimes start enabled stdio commands as child processes; they do not use an HTTP wrapper or background daemon.
- The server does not automatically expose agent tools. Applications must register tools and enforce their own access policy.
- SQLite sessions, model authentication, personas, and mail workflows belong to the coding-agent runtime. They are not requirements for every library.
- The Markdown renderer produces terminal output with ANSI styling. It is not an HTML renderer.

## Agent features

The agents provide schema-validated tools for repository inspection, exact edits, unified patches, shell validation, database access, and scratchpad state. Tool operations are explicit and individually reviewable. Mail workflow tools enforce staging, review, approval, and finalization protections server-side. Small patches keep their rationale and can be checked independently or used for git bisection.

- **Personas and skills**: Define global agent instructions via `AGENTS.md` and extend capabilities using modular, on-demand `SKILL.md` files.
- **SQLite ledger**: Interactions and tool calls are stored in SQLite. Interactive sessions and `sl` use persistent storage; batch mode can use an in-memory database.
- **Session scratchpad**: Maintain a per-session planning buffer with `/scratchpad edit`, `/scratchpad save`, and `read_scratchpad`/`write_scratchpad` tools.
- **Context control**: SQL-backed, per-session accordion history preserves an append-only prompt prefix between resets, so sessions can grow independently while retaining cache-friendly context.
- **Accordion context**: Use `/context <retain_groups> [watermark_tokens]` to grow a cache-friendly prompt prefix, then reset to complete recent groups after the latest actual prompt usage reaches the watermark (defaults: `2`, `350000`). Tool results remain full fidelity up to their configured per-result limit.
- **Mail workflows**: Use [docs/mail_mode.md](docs/mail_mode.md) for a review-first patch workflow. Careful code review remains the authority while each small, discrete patch stays independently reviewable, verifiable, and suitable for git bisection.
- **Models**: Supports OpenAI-compatible Responses endpoints and ChatGPT Plus/Pro OAuth. Routers such as OpenRouter can provide access to other models through compatible endpoints.
- **Hotwords**: Activate a skill for one turn with `hey <skill> <query>`.

## Agent quick start

### Download
The project ships Linux x86-64 and macOS binaries every [release](https://github.com/hsaliak/std_slop/releases). You can directly use them.

### Prerequisites
- C++17 compiler (Clang/GCC)
- [Bazel](https://bazel.build/install) (Bazelisk recommended)
- Git is needed for repository and mail workflows. Initialize the target repository and create an initial commit before using Git mutation tools; it is not needed to use the Markdown library or echo server.

### Build and install
```bash
# Build both agent interfaces
bazel build //:std_slop //:sl

# Optional: add them to your PATH
cp ./bazel-bin/app/std_slop ./bazel-bin/app/sl /usr/local/bin/
```

### Interactive and batch usage
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

### Scripted usage with `sl`

`sl` runs non-interactive prompts and state subcommands. It keeps a persistent database by default; use `--ephemeral` for an in-memory run.

```sh
sl --prompt "Review the authentication flow"
cat build.log | sl --prompt "Explain this failure"
sl session list --json
```

See the [CLI guide](docs/sl.md) for database selection, JSON and schema output, and state subcommands. `sl` uses `--json` and `--schema`; `std_slop` batch mode uses `--output=json` and `--format` or `--format_file`.

Read the [Walkthrough](docs/WALKTHROUGH.md) first for the recommended getting-started flow, authentication setup paths, `config.ini` setup, docs-folder navigation, and `llm_query` subquery/persona configuration. Then use [docs/README.md](docs/README.md) as the docs index for deeper reference material.

### Agent authentication
- OpenAI-compatible API key: set `OPENAI_API_KEY`, optionally combine with `--openai_base_url`, or put both in `config.ini`
- OpenAI OAuth (Responses API): run `std_slop --fetch_openai_oauth_token` or `std_slop --fetch_openai_oauth_device_token`, then start with `--openai_oauth`

### Agent configuration
Both agent interfaces use environment variables or a configuration file.

#### Configuration File
The agent looks for a configuration file at `~/.config/slop/config.ini`. You can also specify a custom path using the `--config` flag.
Keep `slop.db` outside the codebase, or ignore it and its SQLite sidecars (`slop.db-wal` and `slop.db-shm`) in Git. The database stores the context ledger and can contain sensitive data from prompts, tool output, or environment variables.

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
- `SLOP_DEBUG_HTTP=1`: Log HTTP headers and bodies for debugging. These logs can contain credentials and other sensitive data.


## Build and test

Use a C++17 compiler and [Bazel](https://bazel.build/install) (Bazelisk is recommended). Build and test the monorepo:

```sh
bazel build //...
bazel test //...
```

The library quick starts above build selected components. Model credentials are needed for live agent/model calls, not for the library builds or local echo test.

## Code conventions

- C++ Standard: C++17.
- Style: Google C++ Style Guide.
- Exceptions: Disabled (-fno-exceptions).
- Memory: RAII and std::unique_ptr exclusively.
- Error Handling: absl::Status and absl::StatusOr.
- Changes must remain ASan- and TSan-clean.

## Documentation

- **[std::slop Coding Agent](docs/WALKTHROUGH.md)**: Interactive setup, configuration, and first sessions.
- **[sl Command-line Tool](docs/sl.md)**: Scripted prompts, state commands, and MCP configuration.
- **[MCP Client API](docs/mcp-api.md)**: C++ HTTP client, bearer tokens, and OAuth helpers.
- **[MCP Server API](docs/mcp-server.md)**: Stdio server, inline echo example, limits, and security scope.
- **[Agent Database and Schema](docs/SCHEMA.md)**: The SQLite-backed agent runtime and its data model.
- **[Markdown](markdown/README.md)**: Parser and terminal renderer APIs and examples.

- **[Personas & Skills](docs/CONTEXT.md)**: Understanding global context injection and modular skills.
- **[Documentation Guide](docs/README.md)**: Entry point and reading order for the documentation set.
- **[Sessions](docs/SESSIONS.md)**: How context isolation and management work.
- **[Context Management](docs/CONTEXT_MANAGEMENT.md)**: The history and strategy for managing model memory.
- **[MCP User Guide](docs/mcp-slop-userguide.md)**: How `std_slop` registers, authenticates, discovers, and exposes MCP tools.
- **[Subquery Implementation Notes](docs/impl/subqueries.md)**: Design and policy notes for INI-configured `llm_query` specializations.
- **[Fuzzing](docs/fuzzing.md)**: FuzzTest targets, invariants, and how to run/extend the fuzz suite.
- **[Contributing](docs/CONTRIBUTING.md)**: Code style, formatting, and linting guidelines.

## Repository layout

| Package | Role |
| --- | --- |
| `markdown/` | Markdown parsing, syntax highlighting, and ANSI terminal rendering. |
| `mcp/client/` | Outbound HTTP MCP client, authorization helpers, and examples. |
| `mcp/server/` | Inbound stdio MCP server, dispatcher, and echo example. |
| `mcp/` | Shared MCP types, JSON-RPC helpers, and schema validation. |
| `app/` | Entry points for `std_slop` and `sl`. |
| `interface/` | Interactive UI and application control. |
| `core/` | Agent model orchestration, SQLite state, HTTP, and shell support. |
| `tools/` | Agent tool implementations and argument validation. |
| `docs/`, `site/`, `scripts/build_pages.py` | Documentation sources and generated website. |

### Agent runtime modules
The shared agent runtime includes:

- **`database.h`**: Manages the SQLite-backed ledger. Handles persistence for messages, tools, skills, sessions, and usage data.
- **`tool_dispatcher.h`**: Implements a thread-safe execution engine. It dispatches multiple tool calls concurrently while ensuring results are returned in the proper order for the LLM.
- **`cancellation.h`**: Provides a mechanism for interrupting tasks. It supports registering callbacks to kill shell processes or abort HTTP requests.
- **`orchestrator.h`**: high-level interface for model interaction. The Responses API orchestrator manages history windowing and response parsing.
- **`shell_util.h`**: Executes shell commands in a separate process group, with support for live output polling and termination on cancellation.
- **`http_client.h`**: A cancellation-aware HTTP client used for model API calls.

### Agent interface and display
- **`interface/`**: Implements the terminal UI with readline input, ANSI colors, and terminal control sequences.
- **`markdown/`**: Uses Tree-sitter for Markdown parsing and code highlighting (C++, Python, Go, JavaScript, Rust, Bash). The agent UI uses it to render responses; C++ callers can use the library separately.
- **`app/main.cpp`**: The primary event loop. Coordinates between the Orchestrator, ToolDispatcher, and UI.






