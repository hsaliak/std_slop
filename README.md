### FILE: README.md | TOTAL_LINES: 115 | RANGE: 1-115
1: # std::slop
2:   
3: ![std::slop](docs/slop.png)
4: 
5: `std::slop` is a persistent, SQLite-driven C++ CLI agent. It remembers your work through a per-project ledger, providing long-term recall, structured state management, and Built-in Git integration. It's goal is to make the context and its use fully transparent and configurable.
6: 
7: ## ✨ Key Features
8: 
9: - **📖 Ledger-Driven**: All interactions and tool calls are stored in SQLite for persistence and auditability.
10: - **🎛️ Context Control**: Granular control over conversation history via SQL-backed retrieval and rolling windows.
11: - **🏷️ Memo System**: Tag-based knowledge persistence that survives across sessions. Think of these as your project's long-term memory.
12: - **📜 [Lua Control Plane](docs/lua_integration.md)**: Programmatic orchestration via a Lua 5.4 bridge, allowing scripts, staging, and execution.
13: - **📬 [Mail Model](docs/mail_model_impl.md)**: A patch-based iteration workflow for complex features. Patches are prepared on a staging branch, reviewed as atomic units, and only finalized after approval. 
14: - **🤖 Multi-Model**: Supports Google Gemini and OpenAI-compatible APIs (OpenRouter, etc.).
15: 
16: ## 🚀 Quick Start
17: 
18: ### 📋 Prerequisites
19: - C++17 compiler (Clang/GCC)
20: - [Bazel](https://bazel.build/install) (Bazelisk recommended)
21: - **Git**: Targets must be valid git repositories. Usually, a git add and an initial commit is sufficient to trigger all the git enabled features.
22: 
23: ### 🛠️ Build and Install
24: ```bash
25: # Build the binary
26: bazel build //:std_slop
27: 
28: # Optional: Add to your PATH
29: cp ./bazel-bin/std_slop /usr/local/bin/
30: ```
31: 
32: ### ⌨️ Usage
33: `std::slop` works best when it can track a specific project. Initialize a git repository and run it from the root:
34: ```bash
35: mkdir my-project && cd my-project
36: git init
37: std_slop
38: ```
39: 
40: For quick one-off tasks, you can use **Batch Mode**:
41: ```bash
42: std_slop --prompt "Refactor main.cpp to remove all unused includes" 
43: ```
44: Batch mode also takes in `--model` which is useful to specify the model to use and `--session` which is useful to indicate the session the prompt should be executed under. Batch mode works off an in memory sqlite db. If you want the db persisted you can point it to a DB with the `--prompt-db` argument.
45: `/commands` are also supported. 
46: 
47: 
48: Read the [User Guide](docs/USERGUIDE.md) for a detailed understanding of how to use std_slop, or [Walkthrough](docs/WALKTHROUGH.md) to start with something simple.
49: 
50: ### ⚙️ Configuration
51: You can configure `std::slop` using environment variables or a configuration file.
52: 
53: #### Configuration File
54: The agent looks for a configuration file at `~/.config/slop/config.ini`. You can also specify a custom path using the `--config` flag.
55: 
56: ```ini
57: [slop]
58: model = gemini-2.0-flash-exp
59: google_api_key = AIza...
60: # OR
61: openai_api_key = sk-...
62: openai_base_url = https://api.openai.com/v1
63: ```
64: 
65: See [docs/example_config.ini](docs/example_config.ini) for a full list of options.
66: 
67: #### Environment Variables
68: - `GEMINI_API_KEY`: Google API key.
69: - `OPENAI_API_KEY`: OpenAI-compatible API key.
70: - `OPENAI_API_BASE`: Base URL for OpenAI-compatible providers.
71: - `SLOP_DEBUG_HTTP=1`: Enable full verbose logging of all HTTP traffic (headers & bodies).
72: 
73: You can also use Google OAuth login if no keys are provided.
74: 
75: ## 💻 Code
76: 
77: - C++ Standard: C++17.
78: - Style: Google C++ Style Guide.
79: - Exceptions: Disabled (-fno-exceptions).
80: - Memory: RAII and std::unique_ptr exclusively.
81: - Error Handling: absl::Status and absl::StatusOr.
82: - Concurrency: Parallel tool execution is managed through the Lua control plane using `_async` variants and a job-based wait system. This allows for granular control over concurrent operations. (`absl::Mutex`, `absl::Notification`). Thread safety is enforced via Absl thread-safety annotations (`ABSL_GUARDED_BY`) and verified with TSAN tests.
83: - Asan and Tsan clean at all times.
84: 
85: ## 📚 Documentation
86: 
87: - **[User Guide](docs/USERGUIDE.md)**: Detailed commands and workflow tips.
88: - **[Architecture & Schema](docs/SCHEMA.md)**: Understanding the database-driven engine.
89: - **[Sessions](docs/SESSIONS.md)**: How context isolation and management work.
90: - **[Context Management](docs/CONTEXT_MANAGEMENT.md)**: The history and strategy for managing model memory.
91: - **[Walkthrough](docs/WALKTHROUGH.md)**: A step-by-step example of using the agent.
92: - **[Lua Integration](docs/lua_integration.md)**: high-level orchestration and task safety via the Lua bridge.
93: - **[Contributing](docs/CONTRIBUTING.md)**: Code style, formatting, and linting guidelines.
94: 
95: ## 🏗️ Architecture & Codebase Layout
96: 
97: ### `core/` - The Engine
98: The core logic is divided into modules:
99: 
100: - **`database.h`**: Manages the SQLite-backed ledger. Handles persistence for messages, memos, tools, and skills.
101: - **`tool_dispatcher.h`**: Implements a thread-safe execution engine. It dispatches multiple tool calls concurrently while ensuring results are returned in the proper order for the LLM.
102: - **`cancellation.h`**: Provides a mechanism for interrupting tasks. It supports registering callbacks to kill shell processes or abort HTTP requests.
103: - **`orchestrator.h`**: high-level interface for model interaction. Implementations for Gemini and OpenAI manage history windowing and response parsing.
104: - **`shell_util.h`**: Executes shell commands in a separate process group, with support for live output polling and termination on cancellation.
105: - **`http_client.h`**: A minimalist, cancellation-aware HTTP client used for all model API calls.
106: 
107: ### `lua-bridge/` - Orchestration Layer
108: - **`lua_bridge.h`**: Implements the Lua 5.4 environment. Provides the `run_lua` tool and manages the injection of global context (`tools`, `history`, `state`).
109: - **`preamble_lib.lua`**: The embedded standard library for the agent's Lua environment. Implements high-level helpers and the `slop_guard` safety mechanism.
110: 
111: ### Interface & Display
112: - **`interface/`**: Implements the terminal UI. The UI is minimal but clean, uses readline for user input, color codes and ASCII Codes.
113: - **`markdown/`**: Uses `tree-sitter-markdown` to provide syntax highlighting (C++, Python, Go, JS, Rust, Bash) and structured rendering for agent responses. This is a stand alone Markdown  parser / renderer library in C++.
114: - **`main.cpp`**: The primary event loop. Coordinates between the Orchestrator, ToolDispatcher, and UI.
115: 
