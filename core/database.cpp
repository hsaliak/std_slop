#include "core/database.h"

#include <iostream>

#include "absl/container/flat_hash_set.h"
#include "absl/log/log.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"
#include "absl/strings/substitute.h"

#include "core/js_tools_data.h"
#include "core/status_macros.h"
#include "json_utils.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>
namespace slop {
Database::Statement::~Statement() {
  if (stmt_) {
    db_wrapper_->ReturnStatement(sql_, stmt_);
  }
}
absl::Status Database::Statement::BindInt(int index, int value) {
  if (sqlite3_bind_int(stmt_, index, value) != SQLITE_OK) {
    return absl::InternalError("BindInt error: " + std::string(sqlite3_errmsg(db_)));
  }
  return absl::OkStatus();
}
absl::Status Database::Statement::BindInt64(int index, int64_t value) {
  if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) {
    return absl::InternalError("BindInt64 error: " + std::string(sqlite3_errmsg(db_)));
  }
  return absl::OkStatus();
}
absl::Status Database::Statement::BindDouble(int index, double value) {
  if (sqlite3_bind_double(stmt_, index, value) != SQLITE_OK) {
    return absl::InternalError("BindDouble error: " + std::string(sqlite3_errmsg(db_)));
  }
  return absl::OkStatus();
}
absl::Status Database::Statement::BindText(int index, const std::string& value) {
  if (sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    return absl::InternalError("BindText error: " + std::string(sqlite3_errmsg(db_)));
  }
  return absl::OkStatus();
}
absl::Status Database::Statement::BindNull(int index) {
  if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) {
    return absl::InternalError("BindNull error: " + std::string(sqlite3_errmsg(db_)));
  }
  return absl::OkStatus();
}
absl::StatusOr<bool> Database::Statement::Step() {
  int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) return false;
  std::string err = sqlite3_errmsg(db_);
  LOG(ERROR) << "Step error: " << err << " (SQL: " << sql_ << ")";
  return absl::InternalError("Step error: " + err + " (SQL: " + sql_ + ")");
}
absl::Status Database::Statement::Run() {
  auto res = Step();
  if (!res.ok()) return res.status();
  return absl::OkStatus();
}
int Database::Statement::ColumnInt(int index) { return sqlite3_column_int(stmt_, index); }
int64_t Database::Statement::ColumnInt64(int index) { return sqlite3_column_int64(stmt_, index); }
double Database::Statement::ColumnDouble(int index) { return sqlite3_column_double(stmt_, index); }
std::string Database::Statement::ColumnText(int index) {
  const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, index));
  return text ? std::string(text) : "";
}
int Database::Statement::ColumnType(int index) { return sqlite3_column_type(stmt_, index); }
const char* Database::Statement::ColumnName(int index) { return sqlite3_column_name(stmt_, index); }
int Database::Statement::ColumnCount() { return sqlite3_column_count(stmt_); }
absl::StatusOr<std::unique_ptr<Database::Statement>> Database::Prepare(const std::string& sql) {
  absl::MutexLock lock(&mu_);
  sqlite3_stmt* raw_stmt = nullptr;
  auto it = stmt_cache_.find(sql);
  if (it != stmt_cache_.end() && !it->second.empty()) {
    raw_stmt = it->second.back();
    it->second.pop_back();
  } else {
    int rc = sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
      std::string err = sqlite3_errmsg(db_.get());
      LOG(ERROR) << "Prepare error: " << err << " (SQL: " << sql << ")";
      return absl::InternalError("Prepare error: " + err + " (SQL: " + sql + ")");
    }
  }
  return std::make_unique<Statement>(this, db_.get(), sql, raw_stmt);
}
void Database::ReturnStatement(const std::string& sql, sqlite3_stmt* stmt) {
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  absl::MutexLock lock(&mu_);
  stmt_cache_[sql].push_back(stmt);
}
Database::~Database() {
  absl::MutexLock lock(&mu_);
  for (auto& pair : stmt_cache_) {
    for (sqlite3_stmt* stmt : pair.second) {
      sqlite3_finalize(stmt);
    }
  }
}
absl::Status Database::Init(const std::string& db_path) {
  LOG(INFO) << "Initializing database at " << db_path;
  sqlite3* raw_db = nullptr;
  int rc = sqlite3_open_v2(db_path.c_str(), &raw_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                           nullptr);
  if (rc != SQLITE_OK) {
    std::string err = sqlite3_errmsg(raw_db);
    sqlite3_close(raw_db);
    LOG(ERROR) << "Failed to open database: " << err;
    return absl::InternalError("Failed to open database: " + err);
  }
  sqlite3_exec(raw_db, "DROP TABLE IF EXISTS code_search;", nullptr, nullptr, nullptr);
  const char* schema = R"(
    CREATE TABLE IF NOT EXISTS messages (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        session_id TEXT,
        role TEXT CHECK(role IN ('system', 'user', 'assistant', 'tool')),
        content TEXT,
        tool_call_id TEXT,
        status TEXT DEFAULT 'completed',
        created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
        group_id TEXT,
        parsing_strategy TEXT,
        tokens INTEGER DEFAULT 0
    );
    CREATE TABLE IF NOT EXISTS tools (
        name TEXT PRIMARY KEY,
        description TEXT,
        json_schema TEXT,
        is_enabled INTEGER DEFAULT 1,
        call_count INTEGER DEFAULT 0
    );
    CREATE TABLE IF NOT EXISTS skills (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT UNIQUE,
        description TEXT,
        system_prompt_patch TEXT,
        activation_count INTEGER DEFAULT 0
    );
    CREATE TABLE IF NOT EXISTS sessions (
        id TEXT PRIMARY KEY,
        context_size INTEGER DEFAULT 3,
        active_skills TEXT
    );
    CREATE TABLE IF NOT EXISTS usage (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        session_id TEXT,
        model TEXT,
        prompt_tokens INTEGER,
        completion_tokens INTEGER,
        total_tokens INTEGER,
        created_at DATETIME DEFAULT CURRENT_TIMESTAMP
    );
    CREATE TABLE IF NOT EXISTS session_state (
        session_id TEXT PRIMARY KEY,
        state_blob TEXT
    );
        CREATE TABLE IF NOT EXISTS agent_md (path TEXT PRIMARY KEY, content TEXT NOT NULL, updated_at DATETIME DEFAULT CURRENT_TIMESTAMP);
    CREATE TABLE IF NOT EXISTS js_functions (
        name TEXT PRIMARY KEY,
        code TEXT,
        description TEXT,
        json_schema TEXT,
        created_at DATETIME DEFAULT CURRENT_TIMESTAMP
    );
  )";
  rc = sqlite3_exec(raw_db, schema, nullptr, nullptr, nullptr);
  if (rc != SQLITE_OK) {
    std::string err = sqlite3_errmsg(raw_db);
    sqlite3_close(raw_db);
    return absl::InternalError("Schema error: " + err);
  }
  // Enable WAL mode for better concurrency and performance.
  (void)sqlite3_exec(raw_db, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
  (void)sqlite3_exec(raw_db, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);
  (void)sqlite3_exec(raw_db, "PRAGMA busy_timeout = 5000;", nullptr, nullptr, nullptr);
  // Migration: Add tokens column to messages table if it doesn't exist
  (void)sqlite3_exec(raw_db, "ALTER TABLE messages ADD COLUMN tokens INTEGER DEFAULT 0;", nullptr, nullptr, nullptr);
  (void)sqlite3_exec(raw_db, "ALTER TABLE skills ADD COLUMN activation_count INTEGER DEFAULT 0;", nullptr, nullptr,
                     nullptr);
  (void)sqlite3_exec(raw_db, "ALTER TABLE sessions ADD COLUMN active_skills TEXT;", nullptr, nullptr, nullptr);
  (void)sqlite3_exec(raw_db, "ALTER TABLE tools ADD COLUMN call_count INTEGER DEFAULT 0;", nullptr, nullptr, nullptr);
  (void)sqlite3_exec(raw_db, "ALTER TABLE js_functions ADD COLUMN description TEXT;", nullptr, nullptr, nullptr);
  (void)sqlite3_exec(raw_db, "ALTER TABLE js_functions ADD COLUMN json_schema TEXT;", nullptr, nullptr, nullptr);
  // Patch Approval and Settings Tables
  (void)sqlite3_exec(raw_db, R"(
        CREATE TABLE IF NOT EXISTS patch_approvals (
        branch_name TEXT PRIMARY KEY,
        approved_hash TEXT NOT NULL,
        approved_at DATETIME DEFAULT CURRENT_TIMESTAMP
    );
    CREATE TABLE IF NOT EXISTS staging_branches (
        branch_name TEXT PRIMARY KEY,
        parent_branch TEXT NOT NULL,
        created_at DATETIME DEFAULT CURRENT_TIMESTAMP
    );
    CREATE TABLE IF NOT EXISTS settings (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        mode TEXT NOT NULL DEFAULT 'standard'
    );
  )",
                     nullptr, nullptr, nullptr);
  // Initialize settings
  (void)sqlite3_exec(raw_db, "INSERT OR IGNORE INTO settings (id, mode) VALUES (1, 'standard');", nullptr, nullptr,
                     nullptr);
  {
    absl::MutexLock lock(&mu_);
    db_.reset(raw_db);
  }

  // Insert default JS functions into js_functions table
  for (const auto& func : GetDefaultJsFunctions()) {
    std::string sql =
        "INSERT INTO js_functions (name, code, description, json_schema) VALUES (?, ?, ?, ?) "
        "ON CONFLICT(name) DO UPDATE SET code=excluded.code, description=excluded.description, "
        "json_schema=excluded.json_schema;";
    (void)Execute(sql, func.name, func.code, func.description, func.json_schema);
  }

  absl::Status s = RegisterDefaultTools();
  if (!s.ok()) return s;
  s = RegisterDefaultSkills();
  if (!s.ok()) return s;
  return absl::OkStatus();
}
absl::Status Database::RegisterDefaultTools() {
  std::vector<Tool> default_tools = {
      {"run_js",
       "Execute a JavaScript (ES2020+) script acting as a high-level 'control plane' with access to a "
       "'tools' object (supporting async variants), and global variable 'state'; optional "
       "Output and return values are captured.",
       R"({"type":"object","properties":{"script":{"type":"string","description":"The JavaScript script to execute."}},"required":["script"]})",
       true},
      {"llm_query",
       "Executes a synchronous LLM query in a transient, isolated environment. Useful for sub-tasks or analysis.",
       R"({"type":"object","properties":{"query":{"type":"string","description":"The prompt to send to the LLM."}},"required":["query"]})",
       true}};
  // Automatically register all core tools defined in the default_tools list.
  // This ensures the agent always has access to the fundamental building blocks
  // for code manipulation and system interaction.
  for (const auto& t : default_tools) {
    absl::Status s = RegisterTool(t);
    if (!s.ok()) return s;
  }

  return absl::OkStatus();
}
absl::Status Database::RegisterDefaultSkills() {
  std::vector<Skill> default_skills = {
      {0, "planner", "Strategic Tech Lead specialized in architectural decomposition and iterative feature delivery.",
       "You are a Strategic Tech Lead specialized in architectural decomposition. Before planning, always check for "
       "iterative checklist. You MUST NOT implement code; you must provide a plan and request feedback. Your job is "
       "to break down a large or abstract request into a plan that is composed of smaller, iterable tasks. You ask "
       "questions and feedback to refine the plan, you iterate with the user until youa are absolutely convinced that "
       "all details have been finalized. Then and only then do you recommend proceeding with implementation."},
      {0, "dba", "Database Administrator specializing in SQLite schema design, optimization, and data integrity.",
       "As a DBA, you are the steward of the project's data. You focus on efficient schema design, precise query "
       "construction, and maintaining data integrity. When interacting with the database: 1. Always verify schema "
       "before operations. 2. Use transactions for complex updates. 3. Provide clear explanations for schema changes. "
       "4. Optimize for performance while ensuring clarity."},
      {0, "c++_expert", "Enforces strict adherence to project C++ constraints.",
       "C++17, Google Style, no exceptions, RAII/unique_ptr and proactive use of Abseil (absl) for safety and "
       "performance."
       "You strictly avoid complex template metaprogramming or deep inheritance."
       "You ALWAYS run all tests. You ALWAYS ensure affected targets compile."
       "You are a C++ Expert specialized in the std::slop codebase.\nYou MUST adhere to these constraints in every "
       "code change:\n- Language: C++17.\n- Style: Google C++ Style Guide.\n- Exceptions: Strictly disabled "
       "raw new/delete. Use stack allocation where possible.\n- Error Handling: Use absl::Status and absl::StatusOr "
       "for all fallible operations.\n- Abseil: Proactively use Abseil (absl) libraries for strings, containers, and "
       "synchronization wherever they provide benefits over standard or custom implementations.\n- Threading: Avoid "
       "threading and async primitives. If necessary, use absl based primitives with std::thread and provide tsan "
       "tests.\n- Design: Prefer simple, readable code over complex template metaprogramming or deep inheritance.\n"
       "You ALWAYS run all tests. You ALWAYS ensure affected targets compile."},
      {0, "code_reviewer",
       "Multilingual code reviewer enforcing language-specific standards (Google C++, PEP8, etc.) and project "
       "conventions.",
       "You are a strict code reviewer. Your goal is to review code changes against industry-standard style guides and "
       "project conventions.\nStandards to follow:\n- C++: Google C++ Style Guide.\n- Python: PEP 8.\n- Others: "
       "Appropriate de-facto industry standards (e.g., Effective Java, Airbnb JS Style Guide).\nYou do NOT implement "
       "changes. You ONLY provide an annotated set of required changes or comments. Only after explicit user approval "
       "can you proceed with addressing the issues identified. Focus on style, safety, and readability. For new files, "
       "use `git add --intent-to-add` before `git diff`. Always list the files reviewed in your summary."}};
  default_skills.push_back(
      {0, "js_control_plane", "Constrains the agent to use the 'run_js' control plane for all operations.",
       "### Skill: js_control_plane\n"
       "DANGER: You are in **JS CONTROL PLANE** mode.\n"
       "- You MUST NOT use any tools directly EXCEPT for `run_js`.\n"
       "- All other operations (file manipulation, searching, bash execution, etc.) MUST be performed by writing and "
       "executing a JavaScript script via `run_js`.\n"
       "- Use `tools.query_db` from inside JCP scripts only when schema or metadata inspection is needed.\n"
       "- This mode ensures all actions are documented, reproducible, and orchestrated via the control plane.\n"
       "- If you need to search, read files, or apply patches, write a JavaScript script that calls the appropriate "
       "`tools` "
       "functions."});
  default_skills.push_back(
      {0, "run_js", "Expert JavaScript scripter capable of orchestrating complex tasks using the JavaScript bridge.",
       "You are a JavaScript scripting expert. You use 'run_js' to orchestrate complex tasks.\n"
       "### ENVIRONMENT\n"
       "- **'tools'**: Table of all tool functions. Every tool takes a SINGLE table argument "
       "(e.g., `tools.read_file({path='foo.txt'})`).\n"
       "- **'tools.help()'**: Call this early to fetch the JSON API manifest, canonical names, "
       "and aliases.\n"
       "- **No 'history' global**: Conversation history is not exposed as a JS global in this runtime.\n"
       "- Use database APIs when prior messages are needed.\n"
       "- **'state'**: Global context string.\n"
       ""
       "### PARALLELISM\n"
       "Use `tools.execute_bash_async` to launch parallel jobs, then "
       "`job:wait()` to block and collect results.\n"
       "### OUTPUT\n"
       "Use `print()` for debugging/logging. The script's final expression or explicit `return` "
       "value is captured and returned to you. Return a concise user-facing result every turn; "
       ""});
  default_skills.push_back(
      {0, "patcher", "Expert at atomic commits and the \"Mail Model\" workflow.",
       "You are the Patcher, an expert software engineer specialized in the \"Mail Model\" workflow. Your primary "
       "mission is to maintain a high-quality, bisect-safe, and logically factored commit history. You operate as a "
       "remote contributor providing a series of atomic patches for review.\n\n"
       "### 1. CORE PHILOSOPHY\n"
       "- **Bisect-Safety**: Every single commit in the series MUST compile and pass tests. There are no \"broken\" "
       "intermediate states.\n"
       "- **Logical Factoring**: Separate \"refactoring\" from \"feature work\" and \"bug fixes\" into distinct "
       "patches.\n"
       "- **Narrative History**: The commit history should tell a clear story of how the feature was built.\n\n"
       ""### 1.1 GOLDEN PATCH TEMPLATE (Copy/Paste Starter)\n"
       "Use this template for each atomic patch step:\n"
       "```js\n"
       "// @ts-check\n"
       "/** @returns {Promise<any>} */\n"
       "async function main() {\n"
       "  await tools.git_branch_staging({ name: '<SHORT_BRANCH_NAME>' });\n"
       "  // ...perform focused changes...\n"
       "  const commit = await tools.git_commit_patch({\n"
       "    summary: '<SHORT_SUMMARY>',\n"
       "    rationale: '<WHY_THIS_CHANGE>',\n"
       "  });\n"
       "  const verify = await tools.git_verify_series({ command: '<VERIFY_COMMAND>' });\n"
       "  const series = await tools.git_format_patch_series({});\n"
       "  return { ok: true, commit, verify, series };\n"
       "}\n"
       "return await main();\n"
       "```\n\n"
       ### 2. WORKFLOW LIFECYCLE\n"
       "You MUST follow these stages in order and use `run_js` in the JavaScript Control Plane:\n"
       "1. **Initiation**: Use `git_branch_staging` to create a dedicated branch (prefix: `slop/staging/`). NEVER work "
       "directly on `main`.\n"
       "2. **Incremental Development**: Perform a logical unit of work, then use `tools.git_commit_patch` immediately "
       "to capture it. Provide a concise `summary` (50 chars) and a deep `rationale` (Why this? Why now? What "
       "trade-offs?).\n"
       "3. **Verification**: Before presenting to the user, run `tools.git_verify_series`. If any patch fails, you "
       "MUST fix it via `tools.git_reroll_patch` before proceeding.\n"
       "4. **Presentation**: Use `tools.git_format_patch_series` to generate a summary of your work for the user.\n"
       "5. **Review & Reroll**: If the user provides feedback (often via `/review mail` which may contain 'R:' "
       "prefixed comments), apply the requested changes and use `tools.git_reroll_patch` with the specified index. "
       "ALWAYS re-verify after a reroll.\n"
       "6. **Finalization**: When the user provides an \"LGTM\", \"Looks Good\", or explicit approval, use "
       "`tools.git_finalize_series` to land the work.\n\n"
       "### 3. PRECISE TOOL FUNCtiON  USAGE RULES  within the JavaScript control plane\n"
       "- Use `tools.git_branch_staging` as the canonical staging-branch tool.\n"
       "- If `tools.git_create_staging_branch` is present, treat it as a compatibility alias and still prefer `tools.git_branch_staging`.\n"
       "- **tools.git_branch_staging**: Use at the start of every new task.\n"
       "- **tools.git_commit_patch**: Use for every atomic step. Do NOT batch multiple logical changes. ALWAYS include "
       "the returned series summary in your response.\n"
       "- **tools.git_format_patch_series**: Your \"Source of Truth\" for the full series (diffs, rationales). Use it "
       "to present the work for formal review. For immediate status after commits, use the summary returned by the "
       "tool itself.\n"
       "- **tools.git_reroll_patch**: Use ONLY to update an existing patch. Incorporate current workspace changes into "
       "the specified index. Ensure changes are staged or present before calling. ALWAYS include the returned series "
       "summary in your response.\n"
       "- **tools.git_verify_series**: Run after EVERY commit and EVERY reroll. Provide the exact build/test command "
       "relevant to the project (e.g., `bazel test //...`).\n"
       "- **tools.git_finalize_series**: Use only AFTER explicit user approval. It merges and deletes the staging "
       "branch.\n"
       "DO NOT use direct git commands with `io.Popen` to circumvent this workflow.\n\n"
       "### 4. HANDLING REVIEWS (The Inlined \"R:\" Protocol)\n"
       "When the user runs `/review mail`, you will receive a message containing the full patch series with inlined "
       "feedback. Comments starting with `R:` indicate required reworks.\n"
       "- **Contextual Awareness**: If an `R:` comment appears below a `### Patch [n/total] ###` header, it "
       "specifically applies to patch #n.\n"
       "- **Process**: 1. Identify all `R:` comments and their associated patch indices. 2. For each affected patch: "
       "a. Apply the code changes to the workspace. b. Call `tools.git_reroll_patch with the index=n`. 3. After "
       "addressing ALL comments, run `tools.git_verify_series` and inform the user.\n\n"
       "### 5. PROHIBITIONS\n"
       "- NEVER leave uncommitted changes in the workspace.\n"
       "- NEVER use direct `git commit`; use `tools.git_commit_patch`.\n"
       "- NEVER suggest merging if `tools.git_verify_series` has not passed for the entire series.\n"
       "- NEVER merge without explicit approval of the latest patchset.ONLY merge after approval has been explicitly "
       "provided for patchset under development.\n"
       "- If a conflict occurs during reroll, explain clearly and ask for guidance if you cannot resolve it "
       "automatically.\n\n"
       "Stay focused on the commit history. Be precise, technical, and proactive in fixing your own bugs before the "
       "user sees them."});
  default_skills.push_back(
      {0, "delegator",
       "Uses std_slop with the --prompt flag to execute one-off reasoning that does not require existing context.",
       "### THE DELEGATOR\n"
       "You are the Delegator. Your primary strategy is to offload self-contained sub-tasks to independent instances "
       "of `std_slop`. This is highly effective for:\n"
       "1. **Isolated Reasoning**: Tasks that require deep thought but don't need the full conversation history (e.g., "
       "\"Analyze this 100-line function for potential deadlocks\").\n"
       "2. **Context Preservation**: Keeping your main context window clean by delegating exploratory or repetitive "
       "tasks.\n"
       "3. **Parallelism**: While you execute sequentially, you can think of these as independent processes.\n\n"
       "#### WORKFLOW\n"
       "When you identify a task suitable for delegation:\n"
       "1.  **Decompose**: Extract the exact information needed for the sub-task.\n"
       "2.  **Formulate**: Create a clear, detailed prompt for the sub-agent.\n"
       "3.  **Execute**: Use `execute_bash` to run:\n"
       "    `std_slop --prompt \"Your detailed prompt here\"` \n"
       "4.  **Integrate**: Use the output of the command to inform your next steps in the main conversation.\n\n"
       "#### GUIDELINES\n"
       "- ALWAYS provide all necessary code or context within the `--prompt` string. The sub-agent is fresh and has "
       "NO knowledge of this conversation.\n"
       "- Use single quotes or properly escape double quotes in the shell command.\n"
       "- If the task is too large for a single prompt, consider if it's actually suitable for this delegation "
       "model."});
  for (const auto& s : default_skills) {
    absl::Status status = RegisterSkill(s);
    if (!status.ok()) return status;
  }
  return absl::OkStatus();
}
absl::Status Database::Execute(const std::string& sql) { return Execute(sql, {}); }
absl::Status Database::Execute(const std::string& sql, const std::vector<std::string>& params) {
  auto stmt_or = Prepare(sql);
  if (!stmt_or.ok()) return stmt_or.status();
  for (size_t i = 0; i < params.size(); ++i) {
    RETURN_IF_ERROR((*stmt_or)->BindText(i + 1, params[i]));
  }
  return (*stmt_or)->Run();
}
absl::Status Database::AppendMessage(const std::string& session_id, const std::string& role, const std::string& content,
                                     const std::string& tool_call_id, const std::string& status,
                                     const std::string& group_id, const std::string& parsing_strategy, int tokens) {
  // Ensure session exists
  RETURN_IF_ERROR(Execute("INSERT OR IGNORE INTO sessions (id) VALUES (?)", session_id));
  std::string sql =
      "INSERT INTO messages (session_id, role, content, tool_call_id, status, group_id, parsing_strategy, tokens) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
  ASSIGN_OR_RETURN(auto stmt, Prepare(sql));
  RETURN_IF_ERROR(stmt->BindText(1, session_id));
  RETURN_IF_ERROR(stmt->BindText(2, role));
  RETURN_IF_ERROR(stmt->BindText(3, content));
  if (tool_call_id.empty()) {
    RETURN_IF_ERROR(stmt->BindNull(4));
  } else {
    RETURN_IF_ERROR(stmt->BindText(4, tool_call_id));
  }
  RETURN_IF_ERROR(stmt->BindText(5, status));
  if (group_id.empty()) {
    RETURN_IF_ERROR(stmt->BindNull(6));
  } else {
    RETURN_IF_ERROR(stmt->BindText(6, group_id));
  }
  if (parsing_strategy.empty()) {
    RETURN_IF_ERROR(stmt->BindNull(7));
  } else {
    RETURN_IF_ERROR(stmt->BindText(7, parsing_strategy));
  }
  RETURN_IF_ERROR(stmt->BindInt(8, tokens));
  return stmt->Run();
}
absl::Status Database::UpdateMessageStatus(int id, const std::string& status) {
  return Execute("UPDATE messages SET status = ? WHERE id = ?;", status, id);
}
/**
 * @brief Retrieves messages for a specific session, optionally windowed.
 *
 * If window_size > 0, it retrieves messages from the most recent 'window_size' groups.
 * A 'group' typically corresponds to one full interaction (user prompt + assistant response).
 *
 * @param session_id The session to query.
 * @param include_dropped If true, includes messages marked as 'dropped'.
 * @param window_size Number of recent groups to include. 0 for all history.
 * @return absl::StatusOr<std::vector<Message>> A list of messages ordered by time.
 */
absl::StatusOr<std::vector<Database::Message>> Database::GetConversationHistory(const std::string& session_id,
                                                                                bool include_dropped, int window_size) {
  std::string sql;
  std::string drop_filter = include_dropped ? "" : "AND status != 'dropped'";
  if (window_size > 0) {
    // This query retrieves the history with a turn-based windowing logic.
    // Instead of limiting by raw message count, it limits by 'group_id' count.
    // Each 'group_id' represents a full turn (user prompt + multiple tool calls/responses).
    // This ensures that we don't truncate a conversation in the middle of a tool-calling sequence.
    sql = absl::Substitute(
        "SELECT id, session_id, role, content, tool_call_id, status, created_at, group_id, parsing_strategy, tokens "
        "FROM messages WHERE session_id = ? $0 "
        "AND (group_id IS NULL OR group_id IN (SELECT DISTINCT group_id FROM messages WHERE session_id = ? AND "
        "group_id IS NOT NULL $0 ORDER BY created_at DESC, id DESC LIMIT ?)) "
        "ORDER BY created_at ASC, id ASC",
        drop_filter);
  } else {
    sql = absl::Substitute(
        "SELECT id, session_id, role, content, tool_call_id, status, created_at, group_id, parsing_strategy, tokens "
        "FROM messages WHERE session_id = ? $0 "
        "ORDER BY created_at ASC, id ASC",
        drop_filter);
  }
  ASSIGN_OR_RETURN(auto stmt, Prepare(sql));
  RETURN_IF_ERROR(stmt->BindText(1, session_id));
  if (window_size > 0) {
    RETURN_IF_ERROR(stmt->BindText(2, session_id));
    RETURN_IF_ERROR(stmt->BindInt(3, window_size));
  }
  std::vector<Message> history;
  while (true) {
    auto row_or = stmt->Step();
    if (!row_or.ok()) return row_or.status();
    if (!*row_or) break;
    Message m;
    m.id = stmt->ColumnInt(0);
    m.session_id = stmt->ColumnText(1);
    m.role = stmt->ColumnText(2);
    m.content = stmt->ColumnText(3);
    m.tool_call_id = stmt->ColumnText(4);
    m.status = stmt->ColumnText(5);
    m.created_at = stmt->ColumnText(6);
    m.group_id = stmt->ColumnText(7);
    m.parsing_strategy = stmt->ColumnText(8);
    m.tokens = stmt->ColumnInt(9);
    history.push_back(m);
  }
  return history;
}
absl::StatusOr<std::vector<Database::Message>> Database::GetMessagesByGroups(
    const std::vector<std::string>& group_ids) {
  if (group_ids.empty()) return std::vector<Message>();
  std::string placeholders;
  for (size_t i = 0; i < group_ids.size(); ++i) {
    placeholders += (i == 0 ? "?" : ", ?");
  }
  std::string sql =
      "SELECT id, session_id, role, content, tool_call_id, status, created_at, group_id, parsing_strategy, tokens "
      "FROM messages WHERE group_id IN (" +
      placeholders + ") ORDER BY created_at ASC, id ASC";
  ASSIGN_OR_RETURN(auto stmt, Prepare(sql));
  for (size_t i = 0; i < group_ids.size(); ++i) {
    RETURN_IF_ERROR(stmt->BindText(i + 1, group_ids[i]));
  }
  std::vector<Message> messages;
  while (true) {
    auto row_or = stmt->Step();
    if (!row_or.ok()) return row_or.status();
    if (!*row_or) break;
    Message m;
    m.id = stmt->ColumnInt(0);
    m.session_id = stmt->ColumnText(1);
    m.role = stmt->ColumnText(2);
    m.content = stmt->ColumnText(3);
    m.tool_call_id = stmt->ColumnText(4);
    m.status = stmt->ColumnText(5);
    m.created_at = stmt->ColumnText(6);
    m.group_id = stmt->ColumnText(7);
    m.parsing_strategy = stmt->ColumnText(8);
    m.tokens = stmt->ColumnInt(9);
    messages.push_back(m);
  }
  return messages;
}
absl::StatusOr<std::string> Database::GetLastGroupId(const std::string& session_id) {
  std::string sql =
      "SELECT group_id FROM messages WHERE session_id = ? AND group_id IS NOT NULL ORDER BY created_at DESC, id DESC "
      "LIMIT 1";
  ASSIGN_OR_RETURN(auto stmt, Prepare(sql));
  RETURN_IF_ERROR(stmt->BindText(1, session_id));
  auto row_or = stmt->Step();
  if (!row_or.ok()) return row_or.status();
  if (*row_or) {
    return stmt->ColumnText(0);
  }
  return absl::NotFoundError("No group found");
}
absl::Status Database::RecordUsage(const std::string& session_id, const std::string& model, int prompt_tokens,
                                   int completion_tokens) {
  // Ensure session exists
  RETURN_IF_ERROR(Execute("INSERT OR IGNORE INTO sessions (id) VALUES (?)", session_id));
  return Execute(
      "INSERT INTO usage (session_id, model, prompt_tokens, completion_tokens, total_tokens) VALUES (?, ?, ?, ?, ?);",
      session_id, model, prompt_tokens, completion_tokens, prompt_tokens + completion_tokens);
}
absl::StatusOr<Database::TotalUsage> Database::GetTotalUsage(const std::string& session_id) {
  std::string sql = "SELECT SUM(prompt_tokens), SUM(completion_tokens), SUM(total_tokens) FROM usage";
  if (!session_id.empty()) {
    sql += " WHERE session_id = ?";
  }
  ASSIGN_OR_RETURN(auto stmt, Prepare(sql));
  if (!session_id.empty()) {
    RETURN_IF_ERROR(stmt->BindText(1, session_id));
  }
  auto row_or = stmt->Step();
  if (!row_or.ok()) return row_or.status();
  TotalUsage usage = {0, 0, 0};
  if (*row_or) {
    usage.prompt_tokens = stmt->ColumnInt(0);
    usage.completion_tokens = stmt->ColumnInt(1);
    usage.total_tokens = stmt->ColumnInt(2);
  }
  return usage;
}
absl::Status Database::RegisterTool(const Tool& tool) {
  std::string sql =
      "INSERT INTO tools (name, description, json_schema, is_enabled, call_count) VALUES (?, ?, ?, ?, ?) "
      "ON CONFLICT(name) DO UPDATE SET description=excluded.description, json_schema=excluded.json_schema, "
      "is_enabled=excluded.is_enabled;";
  return Execute(sql, tool.name, tool.description, tool.json_schema, tool.is_enabled ? 1 : 0, tool.call_count);
}
absl::StatusOr<std::vector<Database::Tool>> Database::GetEnabledTools() {
  std::string sql = "SELECT name, description, json_schema, is_enabled, call_count FROM tools WHERE is_enabled = 1";
  ASSIGN_OR_RETURN(auto stmt, Prepare(sql));
  std::vector<Tool> tools;
  while (true) {
    auto row_or = stmt->Step();
    if (!row_or.ok()) return row_or.status();
    if (!*row_or) break;
    Tool t;
    t.name = stmt->ColumnText(0);
    t.description = stmt->ColumnText(1);
    t.json_schema = stmt->ColumnText(2);
    t.is_enabled = stmt->ColumnInt(3) != 0;
    t.call_count = stmt->ColumnInt(4);
    tools.push_back(t);
  }
  return tools;
}
absl::Status Database::RegisterSkill(const Skill& skill) {
  return Execute(
      "INSERT OR IGNORE INTO skills (name, description, system_prompt_patch, activation_count) VALUES (?, ?, ?, ?);",
      skill.name, skill.description, skill.system_prompt_patch, skill.activation_count);
}
absl::Status Database::UpdateSkill(const Skill& skill) {
  return Execute("UPDATE skills SET description = ?, system_prompt_patch = ?, activation_count = ? WHERE name = ?;",
                 skill.description, skill.system_prompt_patch, skill.activation_count, skill.name);
}
absl::Status Database::DeleteSkill(const std::string& name_or_id) {
  std::string sql = "DELETE FROM skills WHERE name = ? OR id = ?;";
  int id = 0;
  if (absl::SimpleAtoi(name_or_id, &id)) {
    return Execute(sql, name_or_id, id);
  }
  return Execute(sql, name_or_id, nullptr);
}
absl::StatusOr<std::vector<Database::Skill>> Database::GetSkills() {
  std::string sql = "SELECT id, name, description, system_prompt_patch, activation_count FROM skills";
  ASSIGN_OR_RETURN(auto stmt, Prepare(sql));
  std::vector<Skill> skills;
  while (true) {
    auto row_or = stmt->Step();
    if (!row_or.ok()) return row_or.status();
    if (!*row_or) break;
    Skill s;
    s.id = stmt->ColumnInt(0);
    s.name = stmt->ColumnText(1);
    s.description = stmt->ColumnText(2);
    s.system_prompt_patch = stmt->ColumnText(3);
    s.activation_count = stmt->ColumnInt(4);
    skills.push_back(s);
  }
  return skills;
}
absl::Status Database::IncrementSkillActivationCount(const std::string& name_or_id) {
  std::string sql = "UPDATE skills SET activation_count = activation_count + 1 WHERE name = ? OR id = ?;";
  int id = 0;
  if (absl::SimpleAtoi(name_or_id, &id)) {
    return Execute(sql, name_or_id, id);
  }
  return Execute(sql, name_or_id, nullptr);
}
absl::Status Database::IncrementToolCallCount(const std::string& name) {
  std::string sql = "UPDATE tools SET call_count = call_count + 1 WHERE name = ?;";
  return Execute(sql, name);
}
absl::Status Database::SetActiveSkills(const std::string& session_id, const std::vector<std::string>& skills) {
  // Ensure session exists
  RETURN_IF_ERROR(Execute("INSERT OR IGNORE INTO sessions (id) VALUES (?)", session_id));
  nlohmann::json j = skills;
  return Execute("UPDATE sessions SET active_skills = ? WHERE id = ?;", j.dump(), session_id);
}
absl::StatusOr<std::vector<std::string>> Database::GetActiveSkills(const std::string& session_id) {
  ASSIGN_OR_RETURN(auto stmt, Prepare("SELECT active_skills FROM sessions WHERE id = ?;"));
  RETURN_IF_ERROR(stmt->BindText(1, session_id));
  auto row_or = stmt->Step();
  if (!row_or.ok()) return row_or.status();
  if (*row_or) {
    std::string active_skills_raw = stmt->ColumnText(0);
    if (active_skills_raw.empty()) return std::vector<std::string>();
    auto j_opt = json_parse(active_skills_raw);
    if (!j_opt) return absl::InternalError("Failed to parse active skills");
    auto& j = *j_opt;
    if (!j.is_discarded() && j.is_array()) {
      return json_getter<std::vector<std::string>>::get(j).value_or(std::vector<std::string>{});
    }
  }
  return std::vector<std::string>();
}
absl::Status Database::SetContextWindow(const std::string& session_id, int size) {
  RETURN_IF_ERROR(Execute("INSERT OR IGNORE INTO sessions (id) VALUES (?)", session_id));
  return Execute("UPDATE sessions SET context_size = ? WHERE id = ?;", size, session_id);
}
absl::StatusOr<Database::ContextSettings> Database::GetContextSettings(const std::string& session_id) {
  std::string sql = "SELECT context_size FROM sessions WHERE id = ?";
  ASSIGN_OR_RETURN(auto stmt, Prepare(sql));
  RETURN_IF_ERROR(stmt->BindText(1, session_id));
  auto row_or = stmt->Step();
  if (!row_or.ok()) return row_or.status();
  ContextSettings settings = {5};  // Default
  if (*row_or) {
    settings.size = stmt->ColumnInt(0);
  }
  return settings;
}
absl::Status Database::SetSessionState(const std::string& session_id, const std::string& state_blob) {
  // Ensure session exists
  RETURN_IF_ERROR(Execute("INSERT OR IGNORE INTO sessions (id) VALUES (?)", session_id));
  return Execute("INSERT OR REPLACE INTO session_state (session_id, state_blob) VALUES (?, ?);", session_id,
                 state_blob);
}
/**
 * @brief Retrieves the persisted state blob for a session.
 *
 * Used to store and recover intermediate session state (like partially
 * constructed responses or temporary context) across restarts.
 *
 * @param session_id The session ID.
 * @return absl::StatusOr<std::string> The state blob string, or NotFoundError if missing.
 */
absl::StatusOr<std::string> Database::GetSessionState(const std::string& session_id) {
  std::string sql = "SELECT state_blob FROM session_state WHERE session_id = ?";
  ASSIGN_OR_RETURN(auto stmt, Prepare(sql));
  RETURN_IF_ERROR(stmt->BindText(1, session_id));
  auto row_or = stmt->Step();
  if (!row_or.ok()) return row_or.status();
  if (*row_or) {
    return stmt->ColumnText(0);
  }
  return absl::NotFoundError("Session state not found");
}
absl::Status Database::DeleteSession(const std::string& session_id) {
  RETURN_IF_ERROR(Execute("DELETE FROM messages WHERE session_id = ?;", session_id));
  RETURN_IF_ERROR(Execute("DELETE FROM usage WHERE session_id = ?;", session_id));
  RETURN_IF_ERROR(Execute("DELETE FROM sessions WHERE id = ?;", session_id));
  RETURN_IF_ERROR(Execute("DELETE FROM session_state WHERE session_id = ?;", session_id));
  return absl::OkStatus();
}
absl::Status Database::CloneSession(const std::string& source_id, const std::string& target_id) {
  // Check source exists
  {
    auto stmt_or = Prepare("SELECT 1 FROM sessions WHERE id = ?");
    if (!stmt_or.ok()) return stmt_or.status();
    RETURN_IF_ERROR((*stmt_or)->BindText(1, source_id));
    auto res_or = (*stmt_or)->Step();
    if (!res_or.ok()) return res_or.status();
    if (!*res_or) {
      return absl::NotFoundError(absl::StrCat("Source session '", source_id, "' not found."));
    }
  }
  // Check target doesn't exist
  {
    auto stmt_or = Prepare("SELECT 1 FROM sessions WHERE id = ?");
    if (!stmt_or.ok()) return stmt_or.status();
    RETURN_IF_ERROR((*stmt_or)->BindText(1, target_id));
    auto res_or = (*stmt_or)->Step();
    if (!res_or.ok()) return res_or.status();
    if (*res_or) {
      return absl::AlreadyExistsError(absl::StrCat("Target session '", target_id, "' already exists."));
    }
  }
  RETURN_IF_ERROR(Execute("BEGIN TRANSACTION;"));
  auto rollback_on_failure = [&](absl::Status s) {
    if (!s.ok()) {
      (void)Execute("ROLLBACK;");
    }
    return s;
  };
  absl::Status status = Execute(
      "INSERT INTO sessions (id, context_size, active_skills) "
      "SELECT ?, context_size, active_skills FROM sessions "
      "WHERE id = ?;",
      {target_id, source_id});
  if (!status.ok()) return rollback_on_failure(status);
  status = Execute(
      "INSERT INTO messages (session_id, role, content, tool_call_id, status, "
      "created_at, group_id, parsing_strategy, tokens) "
      "SELECT ?, role, content, tool_call_id, status, created_at, group_id, "
      "parsing_strategy, tokens FROM messages WHERE session_id = ?;",
      {target_id, source_id});
  if (!status.ok()) return rollback_on_failure(status);
  status = Execute(
      "INSERT INTO usage (session_id, model, prompt_tokens, "
      "completion_tokens, total_tokens, created_at) "
      "SELECT ?, model, prompt_tokens, completion_tokens, total_tokens, "
      "created_at FROM usage WHERE session_id = ?;",
      {target_id, source_id});
  if (!status.ok()) return rollback_on_failure(status);
  status = Execute(
      "INSERT INTO session_state (session_id, state_blob) "
      "SELECT ?, state_blob FROM session_state WHERE session_id = ?;",
      {target_id, source_id});
  if (!status.ok()) return rollback_on_failure(status);
  return Execute("COMMIT;");
}
absl::StatusOr<std::string> Database::Query(const std::string& sql) { return Query(sql, {}); }
absl::StatusOr<std::string> Database::Query(const std::string& sql, const std::vector<std::string>& params) {
  auto stmt_or = Prepare(sql);
  if (!stmt_or.ok()) {
    return stmt_or.status();
  }
  auto& stmt = *stmt_or;
  for (size_t i = 0; i < params.size(); ++i) {
    RETURN_IF_ERROR(stmt->BindText(i + 1, params[i]));
  }
  nlohmann::json results = nlohmann::json::array();
  while (true) {
    auto row_or = stmt->Step();
    if (!row_or.ok()) return row_or.status();
    if (!*row_or) break;
    nlohmann::json row = nlohmann::json::object();
    for (int i = 0; i < stmt->ColumnCount(); ++i) {
      std::string name = stmt->ColumnName(i);
      int type = stmt->ColumnType(i);
      if (type == SQLITE_INTEGER)
        row[name] = stmt->ColumnInt64(i);
      else if (type == SQLITE_FLOAT)
        row[name] = stmt->ColumnDouble(i);
      else if (type == SQLITE_NULL)
        row[name] = nullptr;
      else
        row[name] = stmt->ColumnText(i);
    }
    results.push_back(row);
  }
  return json_dump(results);
}
absl::Status Database::SetPatchApproval(const std::string& branch_name, const std::string& hash) {
  auto stmt_or = Prepare(
      "INSERT OR REPLACE INTO patch_approvals (branch_name, approved_hash, approved_at) VALUES (?, ?, "
      "CURRENT_TIMESTAMP)");
  if (!stmt_or.ok()) return stmt_or.status();
  auto stmt = std::move(*stmt_or);
  (void)stmt->BindText(1, branch_name);
  (void)stmt->BindText(2, hash);
  return stmt->Run();
}
absl::StatusOr<std::string> Database::GetPatchApproval(const std::string& branch_name) {
  auto stmt_or = Prepare("SELECT approved_hash FROM patch_approvals WHERE branch_name = ?");
  if (!stmt_or.ok()) return stmt_or.status();
  auto stmt = std::move(*stmt_or);
  (void)stmt->BindText(1, branch_name);
  auto res = stmt->Step();
  if (!res.ok()) return res.status();
  if (!*res) return absl::NotFoundError("No approval found for branch " + branch_name);
  return stmt->ColumnText(0);
}
absl::Status Database::ClearPatchApproval(const std::string& branch_name) {
  auto stmt_or = Prepare("DELETE FROM patch_approvals WHERE branch_name = ?");
  if (!stmt_or.ok()) return stmt_or.status();
  auto stmt = std::move(*stmt_or);
  (void)stmt->BindText(1, branch_name);
  return stmt->Run();
}
absl::StatusOr<bool> Database::SkillExists(const std::string& name_or_id) {
  std::string sql = "SELECT 1 FROM skills WHERE name = ? COLLATE NOCASE OR id = ? LIMIT 1";
  ASSIGN_OR_RETURN(auto stmt, Prepare(sql));
  (void)stmt->BindText(1, name_or_id);
  (void)stmt->BindText(2, name_or_id);
  return stmt->Step();
}
absl::Status Database::SetAgentMd(const std::string& path, const std::string& content) {
  return Execute("INSERT OR REPLACE INTO agent_md (path, content, updated_at) VALUES (?, ?, CURRENT_TIMESTAMP);", path,
                 content);
}
absl::StatusOr<std::string> Database::GetAgentMd(const std::string& path) {
  auto stmt_or = Prepare("SELECT content FROM agent_md WHERE path = ?;");
  if (!stmt_or.ok()) return stmt_or.status();
  auto& stmt = *stmt_or;
  if (auto s = stmt->BindText(1, path); !s.ok()) return s;
  auto row_or = stmt->Step();
  if (!row_or.ok()) return row_or.status();
  if (*row_or) return stmt->ColumnText(0);
  return absl::NotFoundError("No context for: " + path);
}
}  // namespace slop




