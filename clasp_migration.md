### Migration Plan: `std::slop` `main.cpp` to Clasp Common Lisp

**Goal:** Reimplement the core CLI entry point, argument parsing, initialization, and REPL loop of `std::slop` in Clasp Common Lisp, leveraging Lisp's strengths while maintaining the application's overall architecture.

---

#### Phase 1: Project Setup and Foundational Utilities

1.  **Clasp Environment Setup**:
    *   **Action**: Install Clasp and ensure its development environment is configured.
    *   **Consideration**: Familiarize with Clasp's specific features, particularly its C++ interoperability (FFI) and native compilation capabilities.

2.  **ASDF System Definition**:
    *   **Action**: Create a new ASDF (Another System Definition Facility) system for the Lisp `std::slop` project. This will manage dependencies and compilation.
    *   **Dependencies**: Identify and list necessary Common Lisp libraries (e.g., for argument parsing, JSON handling, logging, SQLite bindings).

3.  **Command-Line Argument Parsing**:
    *   **Tool**: Use a robust Common Lisp library like `command-line-arguments` or `cl-getopt`. Clasp also has `ext:command-line-arguments`.
    *   **Action**: Translate all `ABSL_FLAG` definitions from `main.cpp` (e.g., `--session_name`, `--model`, `--google_api_key`, `--openai_api_key`, `--strip_reasoning`) into Lisp argument definitions.
    *   **Output**: Define global Lisp variables (e.g., `*session-name*`, `*openai-api-key*`) to store parsed argument values.

4.  **Logging**:
    *   **Tool**: Integrate a Common Lisp logging library (e.g., `log4cl`).
    *   **Action**: Map existing C++ logging statements (if any, typically `std::cout` or a custom logger) to the chosen Lisp logging system.

#### Phase 2: Core Application Logic Migration (Clasp-Specific)

1.  **Database Integration (SQLite)**:
    *   **Tool**: Utilize `cl-sqlite` or Clasp's FFI to directly interface with the `sqlite3` C library.
    *   **Action**: Translate the `Database` class initialization, connection management, schema creation (`db.Initialize()`, `db.CreateTables()`), and all `query_db` operations from C++ to Common Lisp. This involves converting SQL query strings and result set handling.

2.  **HTTP Client**:
    *   **Tool**: Leverage Clasp's built-in HTTP client capabilities, `drakma` (a popular Lisp HTTP client), or use FFI to bind to an existing C++ `slop::HttpClient` if its API is simple enough to expose.
    *   **Action**: Reimplement `slop::HttpClient`'s GET/POST methods, request assembly, and error handling (e.g., exponential backoff) using Lisp's network primitives or a chosen library.

3.  **Orchestrator Configuration and Builder**:
    *   **Action**: Translate `Orchestrator::Config` and `Orchestrator::Builder` into idiomatic Common Lisp.
    *   **Implementation**: Define Lisp classes (`defclass`) for the `Orchestrator` configuration and a builder object.
    *   **Methods**: Create Lisp functions or generic methods (`defmethod`) that correspond to C++ builder methods like `WithProvider`, `WithModel`, `WithAPIKey`, `WithBaseURL`, `WithStripReasoning`, `WithThrottle`.

4.  **Provider-Specific Logic (OpenAI/Gemini)**:
    *   **Action**: Migrate the logic within `OpenAiOrchestrator` (and `GeminiOrchestrator` if it were implemented in C++).
    *   **JSON Handling**: Translate `nlohmann::json` operations for payload assembly and response parsing to a Lisp JSON library (e.g., `jonathan`, `cl-json`).
    *   **Conditional Logic**: Reimplement the provider selection and setup based on CLI flags or environment variables.

5.  **REPL and Command Handling**:
    *   **Tool**: For an interactive REPL, `cl-readline` can provide command history and editing.
    *   **Action**: Reimplement the main REPL loop where user input is read, slash commands (`/session`, `/context`, `/undo`, etc.) are identified, parsed, and dispatched to corresponding Lisp handler functions.
    *   **Structure**: Map the C++ `CommandHandler` concept to a system of Lisp generic functions or a dispatch table based on command names.

#### Phase 3: Building, Execution, and Error Handling

1.  **Main Entry Point**:
    *   **Action**: Define a top-level Common Lisp function (e.g., `(defun slop-main () ...)`) that orchestrates the entire application flow:
        *   Parse command-line arguments.
        *   Initialize the database and HTTP client.
        *   Create and configure the `Orchestrator` instance.
        *   Start the REPL loop.
    *   **Configuration**: Use Clasp's system definition to make `slop-main` the entry point for an executable.

2.  **Error Handling**:
    *   **Action**: Translate C++ exception handling and `absl::Status` patterns to idiomatic Common Lisp error handling using `handler-case`, `handler-bind`, `unwind-protect`, and `restart-case` for more robust and interactive error recovery.

3.  **Compilation and Packaging**:
    *   **Action**: Use ASDF to define the build process and Clasp's native compilation capabilities to create a standalone executable that includes the Lisp runtime and compiled application code.

#### Key Considerations & Challenges:

*   **C++ FFI**: Decide strategically where to use Clasp's FFI (Foreign Function Interface) to bind to existing C++ code (e.g., `slop::HttpClient` if it's too complex to rewrite) versus rewriting in pure Lisp.
*   **Object-Oriented Translation**: Translating C++ classes and inheritance hierarchies to Common Lisp Object System (CLOS) classes.
*   **Performance**: Clasp compiles Lisp to C++, offering excellent performance, but specific Lisp code might need optimization for critical loops or data processing.
*   **Concurrency**: If `std::slop` uses threads, these need to be translated to Lisp's concurrency primitives (e.g., `bordeaux-threads`).
*   **Testing**: Implement a testing suite in Lisp (e.g., using `fiveam` or `lisp-unit`) to ensure the migrated functionality behaves identically to the C++ version.