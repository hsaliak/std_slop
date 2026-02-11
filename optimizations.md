# Algorithmic Optimization Audit Report

This report outlines algorithmic and data structure optimization opportunities discovered during the audit of the codebase.

## 1. Database Interaction & Persistence

### 1.1. Prepared Statement Caching
- **Location**: `core/database.cpp`, `Database::Prepare`
- **Issue**: Every database operation calls `sqlite3_prepare_v2` to compile SQL into bytecode. This happens on every query, even for frequently used ones like session updates or message retrieval.
- **Optimization**: Implement a `std::unordered_map<std::string, sqlite3_stmt*>` to cache prepared statements. This avoids the overhead of SQL parsing and planning for repetitive operations.

### 1.2. JSON Parsing for Tool Schemas
- **Location**: `core/orchestrator_gemini.cpp`, `GeminiOrchestrator::AssemblePayload`
- **Issue**: The orchestrator parses the JSON schema of *every* available tool from a string on *every* request to the LLM.
- **Optimization**: Store tool schemas as pre-parsed `nlohmann::json` objects in the `Tool` structure or a static cache.

---

## 2. Tool Execution & Management

### 2.1. Tool Dispatching Complexity
- **Location**: `core/tool_executor.cpp`, `ToolExecutor::Execute`
- **Issue**: Tool selection is implemented via a long `if-else if` chain of string comparisons. This is an $O(N)$ lookup where $N$ is the number of tools.
- **Optimization**: Replace the conditional chain with a `std::unordered_map<std::string, ToolHandler>` where `ToolHandler` is a function pointer or `std::function`. This reduces lookup time to $O(1)$.

### 2.2. Skill Membership Checks
- **Location**: `core/tool_executor.cpp`, `Execute` and `use_skill`
- **Issue**: Active skills are stored and retrieved as `std::vector<std::string>`. Membership checks (e.g., checking if `patcher` is active) involve linear searches or repeated database calls.
- **Optimization**: Maintain an in-memory `absl::flat_hash_set<std::string>` for active skills within the `ToolExecutor` session state.

---

## 3. String Processing & Memory Management

### 3.1. Tag Extraction Allocations
- **Location**: `core/database.cpp`, `Database::ExtractTags`
- **Issue**: Uses `absl::StrSplit` to create a `std::vector<std::string>`. This creates a new `std::string` object for every word in the input text, which is very allocation-heavy for large documents.
- **Optimization**: Use `absl::StrSplit` with `absl::string_view` to avoid allocations until the final unique tags are identified and ready for storage.

### 3.2. Redundant JSON Parsing in Parsers
- **Location**: `core/message_parser.cpp`
- **Issue**: `ExtractToolCalls` and `ExtractAssistantText` often operate on the same message content but parse the JSON independently.
- **Optimization**: Use a shared `MessageContext` that caches the parsed `nlohmann::json` object to avoid redundant parsing of long LLM responses.

---

## 4. UI & Interaction Logic

### 4.1. Inefficient Terminal Polling
- **Location**: `core/shell_util.cpp`, `IsEscPressed`
- **Issue**: To check for the Escape key, the code toggles terminal modes (`tcgetattr`/`tcsetattr`) and sets non-blocking I/O on `STDIN` every 100ms. These are relatively expensive system calls.
- **Optimization**: Use a dedicated input thread or enter raw mode once during the interaction loop and use `poll()` or `select()` to check for available bytes without repeated mode switching.

### 4.2. Completion Filtering
- **Location**: `interface/completer.cpp`, `FilterCommands`
- **Issue**: The current implementation filters and then sorts the entire result set.
- **Optimization**: If the command list grows significantly, maintain a pre-sorted list and use `std::equal_range` or a Trie to find matches in $O(\log N)$ or $O(K)$ where $K$ is the prefix length.

---

## Summary of Recommendations
| Priority | Task | Potential Impact |
| :--- | :--- | :--- |
| **High** | Prepared Statement Caching | Significant reduction in DB overhead. |
| **High** | Pre-parse Tool Schemas | Faster request assembly for LLM calls. |
| **Medium** | Map-based Tool Lookup | Cleaner code and $O(1)$ tool dispatching. |
| **Medium** | string_view for Tagging | Lower memory pressure during memoization. |
| **Low** | Input Polling Optimization | Smoother UI performance and less CPU jitter. |
