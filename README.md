### FILE: README.md | TOTAL_LINES: 116 | RANGE: 1-116
1: ### FILE: README.md | TOTAL_LINES: 115 | RANGE: 1-115
2: 1: # std::slop
3: 2:   
4: 3: ![std::slop](docs/slop.png)
5: 4: 
6: 5: `std::slop` is a persistent, SQLite-driven C++ CLI agent. It remembers your work through a per-project ledger, providing long-term recall, structured state management, and Built-in Git integration. It's goal is to make the context and its use fully transparent and configurable.
7: 6: 
8: 7: ## ✨ Key Features
9: 8: 
10: 9: - **📖 Ledger-Driven**: All interactions and tool calls are stored in SQLite for persistence and auditability.
11: 10: - **🎛️ Context Control**: Granular control over conversation history via SQL-backed retrieval and rolling windows.
12: 11: - **🏷️ Memo System**: Tag-based knowledge persistence that survives across sessions. Think of these as your project's long-term memory.
13: 12: - **📜 [Lua Control Plane](docs/lua_integration.md)**: Programmatic orchestration via a Lua 5.4 bridge, allowing scripts, staging, and execution.
14: 13: - **📬 [Mail Model](docs/mail_model_impl.md)**: A patch-based iteration workflow for complex features. Patches are prepared on a staging branch, reviewed as atomic units, and only finalized after approval. 
15: 14: - **🤖 Multi-Model**: Supports Google Gemini and OpenAI-compatible APIs (OpenRouter, etc.).
16: 15: 
17: 16: ## 🚀 Quick Start
18: 17: 
19: 18: ### 📋 Prerequisites
20: 19: - C++17 compiler (Clang/GCC)
21: 20: - [Bazel](https://bazel.build/install) (Bazelisk recommended)
22: 21: - **Git**: Targets must be valid git repositories. Usually, a git add and an initial commit is sufficient to trigger all the git enabled features.
23: 22: 
24: 23: ### 🛠️ Build and Install
25: 24: ```bash
26: 25: # Build the binary
27: 26: bazel build //:std_slop
28: 27: 
29: 28: # Optional: Add to your PATH
30: 29: cp ./bazel-bin/std_slop /usr/local/bin/
31: 30: ```
32: 31: 
33: 32: ### ⌨️ Usage
34: 33: `std::slop` works best when it can track a specific project. Initialize a git repository and run it from the root:
35: 34: ```bash
36: 35: mkdir my-project && cd my-project
37: 36: git init
38: 37: std_slop
39: 38: ```
40: 39: 
41: 40: For quick one-off tasks, you can use **Batch Mode**:
42: 41: ```bash
43: 42: std_slop --prompt "Refactor main.cpp to remove all unused includes" 
44: 43: ```
45: 44: Batch mode also takes in `--model` which is useful to specify the model to use and `--session` which is useful to indicate the session the prompt should be executed under. Batch mode works off an in memory sqlite db. If you want the db persisted you can point it to a DB with the `--prompt-db` argument.
46: 45: `/commands` are also supported. 
47: 46: 
48: 47: 
49: 48: Read the [User Guide](docs/USERGUIDE.md) for a detailed understanding of how to use std_slop, or [Walkthrough](docs/WALKTHROUGH.md) to start with something simple.
50: 49: 
51: 50: ### ⚙️ Configuration
52: 51: You can configure `std::slop` using environment variables or a configuration file.
53: 52: 
54: 53: #### Configuration File
55: 54: The agent looks for a configuration file at `~/.config/slop/config.ini`. You can also specify a custom path using the `--config` flag.
56: 55: 
57: 56: ```ini
58: 57: [slop]
59: 58: model = gemini-2.0-flash-exp
60: 59: google_api_key = AIza...
61: 60: # OR
62: 61: openai_api_key = sk-...
63: 62: openai_base_url = https://api.openai.com/v1
64: 63: ```
65: 64: 
66: 65: See [docs/example_config.ini](docs/example_config.ini) for a full list of options.
67: 66: 
68: 67: #### Environment Variables
69: 68: - `GEMINI_API_KEY`: Google API key.
70: 69: - `OPENAI_API_KEY`: OpenAI-compatible API key.
71: 70: - `OPENAI_API_BASE`: Base URL for OpenAI-compatible providers.
72: 71: - `SLOP_DEBUG_HTTP=1`: Enable full verbose logging of all HTTP traffic (headers & bodies).
73: 72: 
74: 73: You can also use Google OAuth login if no keys are provided.
75: 74: 
76: 75: ## 💻 Code
77: 76: 
78: 77: - C++ Standard: C++17.
79: 78: - Style: Google C++ Style Guide.
80: 79: - Exceptions: Disabled (-fno-exceptions).
81: 80: - Memory: RAII and std::unique_ptr exclusively.
82: 81: - Error Handling: absl::Status and absl::StatusOr.
83: 82: - Concurrency: Parallel tool execution is managed through the Lua control plane using `_async` variants and a job-based wait system. This allows for granular control over concurrent operations. (`absl::Mutex`, `absl::Notification`). Thread safety is enforced via Absl thread-safety annotations (`ABSL_GUARDED_BY`) and verified with TSAN tests.
84: 83: - Asan and Tsan clean at all times.
85: 84: 
86: 85: ## 📚 Documentation
87: 86: 
88: 87: - **[User Guide](docs/USERGUIDE.md)**: Detailed commands and workflow tips.
89: 88: - **[Architecture & Schema](docs/SCHEMA.md)**: Understanding the database-driven engine.
90: 89: - **[Sessions](docs/SESSIONS.md)**: How context isolation and management work.
91: 90: - **[Context Management](docs/CONTEXT_MANAGEMENT.md)**: The history and strategy for managing model memory.
92: 91: - **[Walkthrough](docs/WALKTHROUGH.md)**: A step-by-step example of using the agent.
93: 92: - **[Lua Integration](docs/lua_integration.md)**: high-level orchestration and task safety via the Lua bridge.
94: 93: - **[Contributing](docs/CONTRIBUTING.md)**: Code style, formatting, and linting guidelines.
95: 94: 
96: 95: ## 🏗️ Architecture & Codebase Layout
97: 96: 
98: 97: ### `core/` - The Engine
99: 98: The core logic is divided into modules:
100: 99: 
101: 100: - **`database.h`**: Manages the SQLite-backed ledger. Handles persistence for messages, memos, tools, and skills.
102: 101: - **`tool_dispatcher.h`**: Implements a thread-safe execution engine. It dispatches multiple tool calls concurrently while ensuring results are returned in the proper order for the LLM.
103: 102: - **`cancellation.h`**: Provides a mechanism for interrupting tasks. It supports registering callbacks to kill shell processes or abort HTTP requests.
104: 103: - **`orchestrator.h`**: high-level interface for model interaction. Implementations for Gemini and OpenAI manage history windowing and response parsing.
105: 104: - **`shell_util.h`**: Executes shell commands in a separate process group, with support for live output polling and termination on cancellation.
106: 105: - **`http_client.h`**: A minimalist, cancellation-aware HTTP client used for all model API calls.
107: 106: 
108: 107: ### `lua-bridge/` - Orchestration Layer
109: 108: - **`lua_bridge.h`**: Implements the Lua 5.4 environment. Provides the `run_lua` tool and manages the injection of global context (`tools`, `history`, `state`).
110: 109: - **`preamble_lib.lua`**: The embedded standard library for the agent's Lua environment. Implements high-level helpers and the `slop_guard` safety mechanism.
111: 110: 
112: 111: ### Interface & Display
113: 112: - **`interface/`**: Implements the terminal UI. The UI is minimal but clean, uses readline for user input, color codes and ASCII Codes.
114: 113: - **`markdown/`**: Uses `tree-sitter-markdown` to provide syntax highlighting (C++, Python, Go, JS, Rust, Bash) and structured rendering for agent responses. This is a stand alone Markdown  parser / renderer library in C++.
115: 114: - **`main.cpp`**: The primary event loop. Coordinates between the Orchestrator, ToolDispatcher, and UI.
116: 115: 
