#include "core/database.h"

#include "core/builtin_skills_data.h"

#include <iostream>

#include "absl/container/flat_hash_set.h"
#include "absl/log/log.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"
#include "absl/strings/substitute.h"

#include "core/status_macros.h"
#include "json_utils.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>
namespace slop {

namespace {
constexpr size_t kMaxCachedStatementsPerSql = 8;

absl::Status RollbackTransaction(Database* db, const absl::Status& status) {
  (void)db->Execute("ROLLBACK;");
  return status;
}

absl::Status SessionExists(Database* db, const std::string& session_id) {
  auto stmt_or = db->Prepare("SELECT 1 FROM sessions WHERE id = ?");
  if (!stmt_or.ok()) return stmt_or.status();
  RETURN_IF_ERROR((*stmt_or)->BindText(1, session_id));
  auto res_or = (*stmt_or)->Step();
  if (!res_or.ok()) return res_or.status();
  if (!*res_or) return absl::NotFoundError("Session not found: " + session_id);
  return absl::OkStatus();
}

absl::Status SessionDoesNotExist(Database* db, const std::string& session_id) {
  auto stmt_or = db->Prepare("SELECT 1 FROM sessions WHERE id = ?");
  if (!stmt_or.ok()) return stmt_or.status();
  RETURN_IF_ERROR((*stmt_or)->BindText(1, session_id));
  auto res_or = (*stmt_or)->Step();
  if (!res_or.ok()) return res_or.status();
  if (*res_or) return absl::AlreadyExistsError("Target session already exists: " + session_id);
  return absl::OkStatus();
}

absl::StatusOr<Database::Message> LastMessageForGroup(Database* db, const std::string& session_id,
                                                      const std::string& group_id) {
  auto stmt_or = db->Prepare(
      "SELECT id, session_id, role, content, tool_call_id, status, created_at, group_id, parsing_strategy, tokens "
      "FROM messages WHERE session_id = ? AND group_id = ? ORDER BY created_at DESC, id DESC LIMIT 1");
  if (!stmt_or.ok()) return stmt_or.status();
  RETURN_IF_ERROR((*stmt_or)->BindText(1, session_id));
  RETURN_IF_ERROR((*stmt_or)->BindText(2, group_id));
  auto res_or = (*stmt_or)->Step();
  if (!res_or.ok()) return res_or.status();
  if (!*res_or) return absl::NotFoundError("Group not found in session: " + group_id);
  Database::Message message;
  message.id = (*stmt_or)->ColumnInt(0);
  message.session_id = (*stmt_or)->ColumnText(1);
  message.role = (*stmt_or)->ColumnText(2);
  message.content = (*stmt_or)->ColumnText(3);
  message.tool_call_id = (*stmt_or)->ColumnText(4);
  message.status = (*stmt_or)->ColumnText(5);
  message.created_at = (*stmt_or)->ColumnText(6);
  message.group_id = (*stmt_or)->ColumnText(7);
  message.parsing_strategy = (*stmt_or)->ColumnText(8);
  message.tokens = (*stmt_or)->ColumnInt(9);
  return message;
}
}  // namespace

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
  absl::MutexLock lock(mu_);
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
  absl::MutexLock lock(mu_);
  auto& cached = stmt_cache_[sql];
  if (cached.size() >= kMaxCachedStatementsPerSql) {
    sqlite3_finalize(stmt);
    return;
  }
  cached.push_back(stmt);
}
Database::~Database() {
  absl::MutexLock lock(mu_);
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
        call_count INTEGER DEFAULT 0,
        is_top_level INTEGER DEFAULT 1
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
        accordion_retain_groups INTEGER NOT NULL DEFAULT 2,
        accordion_watermark_tokens INTEGER NOT NULL DEFAULT 350000,
        accordion_epoch_start_group_id TEXT,
        active_skills TEXT
    );
    CREATE TABLE IF NOT EXISTS usage (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        session_id TEXT,
        model TEXT,
        prompt_tokens INTEGER,
        completion_tokens INTEGER,
        total_tokens INTEGER,
        cached_prompt_tokens INTEGER NOT NULL DEFAULT 0,
        cache_write_prompt_tokens INTEGER NOT NULL DEFAULT 0,
        created_at DATETIME DEFAULT CURRENT_TIMESTAMP
    );
    CREATE TABLE IF NOT EXISTS scratchpads (
        session_id TEXT PRIMARY KEY,
        content TEXT NOT NULL DEFAULT '',
        updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,
        FOREIGN KEY(session_id) REFERENCES sessions(id) ON DELETE CASCADE
    );
        CREATE TABLE IF NOT EXISTS agent_md (path TEXT PRIMARY KEY, content TEXT NOT NULL, updated_at DATETIME DEFAULT CURRENT_TIMESTAMP);
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
  (void)sqlite3_exec(raw_db, "ALTER TABLE tools ADD COLUMN is_top_level INTEGER DEFAULT 1;", nullptr, nullptr,
                     nullptr);
  (void)sqlite3_exec(raw_db,
                     "CREATE INDEX IF NOT EXISTS idx_messages_session_created_id "
                     "ON messages(session_id, created_at, id);",
                     nullptr, nullptr, nullptr);
  (void)sqlite3_exec(raw_db,
                     "CREATE INDEX IF NOT EXISTS idx_messages_group_created_id ON messages(group_id, created_at, id);",
                     nullptr, nullptr, nullptr);
  (void)sqlite3_exec(raw_db,
                     "CREATE INDEX IF NOT EXISTS idx_messages_session_created_id_desc_nonnull_group ON "
                     "messages(session_id, created_at DESC, id DESC) WHERE group_id IS NOT NULL;",
                     nullptr, nullptr, nullptr);
  (void)sqlite3_exec(raw_db,
                     "CREATE INDEX IF NOT EXISTS idx_messages_session_created_id_not_dropped ON "
                     "messages(session_id, created_at, id) WHERE status != 'dropped';",
                     nullptr, nullptr, nullptr);
  (void)sqlite3_exec(raw_db,
                     "CREATE INDEX IF NOT EXISTS idx_messages_session_group_created_id_desc_not_dropped ON "
                     "messages(session_id, group_id, created_at DESC, id DESC) "
                     "WHERE group_id IS NOT NULL AND status != 'dropped';",
                     nullptr, nullptr, nullptr);
  (void)sqlite3_exec(raw_db,
                     "ALTER TABLE usage ADD COLUMN cached_prompt_tokens INTEGER NOT NULL DEFAULT 0;",
                     nullptr, nullptr, nullptr);
  (void)sqlite3_exec(raw_db,
                     "ALTER TABLE usage ADD COLUMN cache_write_prompt_tokens INTEGER NOT NULL DEFAULT 0;",
                     nullptr, nullptr, nullptr);
  // Migrate existing session databases to accordion context settings. The
  // deprecated context_size column is intentionally ignored after migration.
  (void)sqlite3_exec(raw_db,
                     "ALTER TABLE sessions ADD COLUMN accordion_retain_groups INTEGER NOT NULL DEFAULT 2;",
                     nullptr, nullptr, nullptr);
  (void)sqlite3_exec(raw_db,
                     "ALTER TABLE sessions ADD COLUMN accordion_watermark_tokens INTEGER NOT NULL DEFAULT 350000;",
                     nullptr, nullptr, nullptr);
  (void)sqlite3_exec(raw_db, "ALTER TABLE sessions ADD COLUMN accordion_epoch_start_group_id TEXT;", nullptr,
                     nullptr, nullptr);
  // Session state is carried by assistant messages in conversation history.
  (void)sqlite3_exec(raw_db, "DROP TABLE IF EXISTS session_state;", nullptr, nullptr, nullptr);
  // The JavaScript control plane has been retired.
  (void)sqlite3_exec(raw_db, "DROP TABLE IF EXISTS js_functions;", nullptr, nullptr, nullptr);
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
    absl::MutexLock lock(mu_);
    db_.reset(raw_db);
  }

  absl::Status s = RegisterDefaultTools();
  if (!s.ok()) return s;
  s = RegisterDefaultSkills();
  if (!s.ok()) return s;
  return absl::OkStatus();
}
absl::Status Database::RegisterDefaultTools() {
  RETURN_IF_ERROR(Execute("DELETE FROM tools WHERE name IN ('patch_tool', 'persist_function', 'run_js')"));
  std::vector<Tool> default_tools = {
      {"query_db", "Execute a SQL query against the internal SQLite database.",
       R"({"type":"object","properties":{"sql":{"type":"string"},"params":{"type":"array","items":{}}},"required":["sql"]})",
       true},
      {"read_file", "Read file content with optional line range and line numbers.",
       R"({"type":"object","properties":{"path":{"type":"string"},"start_line":{"type":"integer"},"end_line":{"type":"integer"},"line_numbers":{"type":"boolean"}}})",
       true, 0, true},
      {"list_directory", "List files and folders in a directory.",
       R"({"type":"object","properties":{"path":{"type":"string"},"depth":{"type":["integer","string"]},"include_ignored":{"type":"boolean"}}})",
       true, 0, true},
      {"describe_db", "Describe database schema objects and columns.", R"({"type":"object","properties":{}})", true},
      {"grep", "Search for a pattern in files.",
       R"({"type":"object","properties":{"pattern":{"type":"string"},"path":{"type":"string"},"context":{"type":["integer","string"]},"limit":{"type":["integer","string"]},"include_ignored":{"type":"boolean"},"fixed_strings":{"type":"boolean"}},"required":["pattern"]})",
       true, 0, true},
      {"execute_bash", "Execute a shell command.",
       R"({"type":"object","properties":{"command":{"type":"string"},"cwd":{"type":"string"},"allow_nonzero_exit":{"type":"boolean"},"timeout_seconds":{"type":"integer","minimum":0,"default":180,"description":"Maximum wall-clock time in seconds before the command is terminated. Set to 0 to disable timeout."}},"required":["command"]})",
       true, 0, true},
      {"edit_tool", "Apply exact text edits to a file.",
       R"({"type":"object","properties":{"path":{"type":"string"},"edits":{"type":"array","items":{"type":"object","properties":{"op":{"type":"string","enum":["replace","insert_before","insert_after","delete"]},"find":{"type":"string"},"text":{"type":"string"},"which":{"oneOf":[{"type":"string","enum":["only","first","last"]},{"type":"integer","minimum":0}]}},"required":["op","find"]}}},"required":["path","edits"]})",
       true, 0, true},
      {"write_file", "Create or overwrite a file.",
       R"({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]})",
       true, 0, true},
      {"read_scratchpad", "Read scratchpad content for the active session.",
       R"({"type":"object","properties":{}})",
       true},
      {"write_scratchpad", "Write scratchpad content for the active session.",
       R"({"type":"object","properties":{"content":{"type":"string"}},"required":["content"]})",
       true},
      {"use_skill", "Activate or run a skill by name.",
       R"({"type":"object","properties":{"name":{"type":"string"},"action":{"type":"string","enum":["activate","deactivate"]}},"required":["name"]})",
       true},
      {"git_create_staging_branch", "Create or switch to a staging branch in mail mode.",
       R"({"type":"object","properties":{"name":{"type":"string"},"base_branch":{"type":"string"}},"required":["name"]})",
       true, 0, true},
      {"git_commit_patch", "Commit a patch with a message in mail mode.",
       R"({"type":"object","properties":{"summary":{"type":"string"},"rationale":{"type":"string"}},"required":["summary"]})",
       true, 0, true},
      {"git_format_patch_series", "Generate a patch series for review.",
       R"({"type":"object","properties":{"base_branch":{"type":"string"}}})", true, 0, true},
      {"git_reroll_patch", "Reroll an existing patch series.",
       R"({"type":"object","properties":{"index":{"type":["integer","string"]},"base_branch":{"type":"string"}}})",
       true, 0, true},
      {"git_verify_series", "Verify each commit in a patch series by running a command per commit.",
       R"({"type":"object","properties":{"command":{"type":"string"},"base_branch":{"type":"string"}},"required":["command"]})",
       true, 0, true},
      {"git_finalize_series", "Finalize a patch series workflow.",
       R"({"type":"object","properties":{"target_branch":{"type":"string"}}})", true, 0, true},
      {"llm_query",
       "Executes a synchronous LLM query in a transient, isolated environment. Useful for sub-tasks or analysis.",
       R"({"type":"object","properties":{"query":{"type":"string","description":"The prompt to send to the LLM."}},"required":["query"]})",
       true},
      {"ask_user", "Prompt the human operator for input when the agent needs clarification or a decision.",
       R"({"type":"object","properties":{"prompt":{"type":"string","description":"Optional message to display when requesting input."}}})",
       true, 0, true}};
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
  for (const auto& s : kBuiltinSkills) {
    absl::Status status = RegisterSkill(
        {0, s.name, s.description, s.system_prompt_patch, 0});
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
                                   int completion_tokens, int cached_prompt_tokens, int cache_write_prompt_tokens) {
  // Ensure session exists
  RETURN_IF_ERROR(Execute("INSERT OR IGNORE INTO sessions (id) VALUES (?)", session_id));
  return Execute(
      "INSERT INTO usage (session_id, model, prompt_tokens, completion_tokens, total_tokens, cached_prompt_tokens, "
      "cache_write_prompt_tokens) VALUES (?, ?, ?, ?, ?, ?, ?);",
      session_id, model, prompt_tokens, completion_tokens, prompt_tokens + completion_tokens, cached_prompt_tokens,
      cache_write_prompt_tokens);
}
absl::StatusOr<std::optional<int>> Database::GetLatestPromptTokens(const std::string& session_id) {
  ASSIGN_OR_RETURN(auto stmt, Prepare("SELECT prompt_tokens FROM usage WHERE session_id = ? "
                                      "ORDER BY created_at DESC, id DESC LIMIT 1"));
  RETURN_IF_ERROR(stmt->BindText(1, session_id));
  ASSIGN_OR_RETURN(bool has_row, stmt->Step());
  if (!has_row) return std::optional<int>();
  return std::optional<int>(stmt->ColumnInt(0));
}
absl::StatusOr<Database::TotalUsage> Database::GetTotalUsage(const std::string& session_id) {
  std::string sql =
      "SELECT SUM(prompt_tokens), SUM(completion_tokens), SUM(total_tokens), SUM(cached_prompt_tokens), "
      "SUM(cache_write_prompt_tokens) FROM usage";
  if (!session_id.empty()) {
    sql += " WHERE session_id = ?";
  }
  ASSIGN_OR_RETURN(auto stmt, Prepare(sql));
  if (!session_id.empty()) {
    RETURN_IF_ERROR(stmt->BindText(1, session_id));
  }
  auto row_or = stmt->Step();
  if (!row_or.ok()) return row_or.status();
  TotalUsage usage = {0, 0, 0, 0, 0};
  if (*row_or) {
    usage.prompt_tokens = stmt->ColumnInt(0);
    usage.completion_tokens = stmt->ColumnInt(1);
    usage.total_tokens = stmt->ColumnInt(2);
    usage.cached_prompt_tokens = stmt->ColumnInt(3);
    usage.cache_write_prompt_tokens = stmt->ColumnInt(4);
  }
  return usage;
}
absl::Status Database::RegisterTool(const Tool& tool) {
  std::string sql =
      "INSERT INTO tools (name, description, json_schema, is_enabled, call_count, is_top_level) "
      "VALUES (?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(name) DO UPDATE SET description=excluded.description, json_schema=excluded.json_schema, "
      "is_enabled=excluded.is_enabled, is_top_level=excluded.is_top_level;";
  return Execute(sql, tool.name, tool.description, tool.json_schema, tool.is_enabled ? 1 : 0, tool.call_count,
                 tool.is_top_level ? 1 : 0);
}

absl::StatusOr<std::vector<Database::Tool>> Database::GetEnabledTools() {
  std::string sql =
      "SELECT name, description, json_schema, is_enabled, call_count, is_top_level "
      "FROM tools WHERE is_enabled = 1";
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
    t.is_top_level = stmt->ColumnInt(5) != 0;
    tools.push_back(t);
  }
  return tools;
}

absl::StatusOr<std::vector<Database::Tool>> Database::GetTopLevelTools() {
  std::string sql =
      "SELECT name, description, json_schema, is_enabled, call_count, is_top_level "
      "FROM tools WHERE is_enabled = 1 AND is_top_level = 1 ORDER BY name ASC";
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
    t.is_top_level = stmt->ColumnInt(5) != 0;
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
  std::string sql = "SELECT id, name, description, system_prompt_patch, activation_count FROM skills ORDER BY name ASC";
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
absl::Status Database::SetAccordionContextSettings(const std::string& session_id, int retain_groups,
                                                    int watermark_tokens) {
  if (retain_groups < 1) {
    return absl::InvalidArgumentError("Accordion retain_groups must be at least 1");
  }
  if (watermark_tokens < 1) {
    return absl::InvalidArgumentError("Accordion watermark_tokens must be positive");
  }
  RETURN_IF_ERROR(Execute("INSERT OR IGNORE INTO sessions (id) VALUES (?)", session_id));
  return Execute("UPDATE sessions SET accordion_retain_groups = ?, accordion_watermark_tokens = ?, "
                 "accordion_epoch_start_group_id = NULL WHERE id = ?;",
                 std::to_string(retain_groups), std::to_string(watermark_tokens), session_id);
}
absl::StatusOr<Database::AccordionContextSettings> Database::GetAccordionContextSettings(
    const std::string& session_id) {
  ASSIGN_OR_RETURN(auto stmt, Prepare("SELECT accordion_retain_groups, accordion_watermark_tokens, "
                                      "COALESCE(accordion_epoch_start_group_id, '') FROM sessions WHERE id = ?"));
  RETURN_IF_ERROR(stmt->BindText(1, session_id));
  auto row_or = stmt->Step();
  if (!row_or.ok()) return row_or.status();
  AccordionContextSettings settings;
  if (*row_or) {
    settings.retain_groups = stmt->ColumnInt(0);
    settings.watermark_tokens = stmt->ColumnInt(1);
    settings.epoch_start_group_id = stmt->ColumnText(2);
  }
  return settings;
}
absl::Status Database::SetAccordionEpochStartGroup(const std::string& session_id, const std::string& group_id) {
  RETURN_IF_ERROR(Execute("INSERT OR IGNORE INTO sessions (id) VALUES (?)", session_id));
  return Execute("UPDATE sessions SET accordion_epoch_start_group_id = ? WHERE id = ?;", group_id, session_id);
}
absl::StatusOr<std::vector<std::string>> Database::GetSessionGroupIdsFrom(
    const std::string& session_id, const std::string& inclusive_start_group_id) {
  std::string sql =
      "SELECT group_id FROM messages WHERE session_id = ? AND status != 'dropped' AND group_id IS NOT NULL "
      "AND group_id != ''";
  if (!inclusive_start_group_id.empty()) {
    sql += " AND id >= (SELECT MIN(id) FROM messages WHERE session_id = ? AND group_id = ?)";
  }
  sql += " GROUP BY group_id ORDER BY MIN(id) ASC";
  ASSIGN_OR_RETURN(auto stmt, Prepare(sql));
  RETURN_IF_ERROR(stmt->BindText(1, session_id));
  if (!inclusive_start_group_id.empty()) {
    RETURN_IF_ERROR(stmt->BindText(2, session_id));
    RETURN_IF_ERROR(stmt->BindText(3, inclusive_start_group_id));
  }
  std::vector<std::string> group_ids;
  while (true) {
    auto row_or = stmt->Step();
    if (!row_or.ok()) return row_or.status();
    if (!*row_or) break;
    group_ids.push_back(stmt->ColumnText(0));
  }
  return group_ids;
}
absl::StatusOr<std::vector<std::string>> Database::GetLastSessionGroupIds(const std::string& session_id,
                                                                             int count) {
  if (count < 1) return absl::InvalidArgumentError("Group count must be positive");
  ASSIGN_OR_RETURN(auto stmt, Prepare("SELECT group_id FROM (SELECT group_id, MAX(id) AS last_id FROM messages "
                                      "WHERE session_id = ? AND status != 'dropped' AND group_id IS NOT NULL "
                                      "AND group_id != '' GROUP BY group_id ORDER BY last_id DESC LIMIT ?) "
                                      "ORDER BY last_id ASC"));
  RETURN_IF_ERROR(stmt->BindText(1, session_id));
  RETURN_IF_ERROR(stmt->BindInt(2, count));
  std::vector<std::string> group_ids;
  while (true) {
    auto row_or = stmt->Step();
    if (!row_or.ok()) return row_or.status();
    if (!*row_or) break;
    group_ids.push_back(stmt->ColumnText(0));
  }
  return group_ids;
}

absl::Status Database::SetScratchpad(const std::string& session_id, const std::string& content) {
  RETURN_IF_ERROR(Execute("INSERT OR IGNORE INTO sessions (id) VALUES (?)", session_id));
  return Execute(
      "INSERT INTO scratchpads (session_id, content, updated_at) VALUES (?, ?, CURRENT_TIMESTAMP) "
      "ON CONFLICT(session_id) DO UPDATE SET content = excluded.content, updated_at = CURRENT_TIMESTAMP;",
      session_id, content);
}

absl::StatusOr<std::string> Database::GetScratchpad(const std::string& session_id) {
  ASSIGN_OR_RETURN(auto stmt, Prepare("SELECT content FROM scratchpads WHERE session_id = ?"));
  RETURN_IF_ERROR(stmt->BindText(1, session_id));
  auto row_or = stmt->Step();
  if (!row_or.ok()) return row_or.status();
  if (!*row_or) return std::string{};
  return stmt->ColumnText(0);
}

absl::StatusOr<std::string> Database::GetLastAssistantMessage(const std::string& session_id) {
  ASSIGN_OR_RETURN(auto stmt,
                   Prepare("SELECT content FROM messages WHERE session_id = ? AND role = 'assistant' "
                           "ORDER BY id DESC LIMIT 1"));
  RETURN_IF_ERROR(stmt->BindText(1, session_id));
  auto row_or = stmt->Step();
  if (!row_or.ok()) return row_or.status();
  if (!*row_or) {
    return absl::NotFoundError("No assistant message found for this session.");
  }
  return stmt->ColumnText(0);
}

absl::Status Database::DeleteSession(const std::string& session_id) {
  RETURN_IF_ERROR(Execute("DELETE FROM messages WHERE session_id = ?;", session_id));
  RETURN_IF_ERROR(Execute("DELETE FROM usage WHERE session_id = ?;", session_id));
  RETURN_IF_ERROR(Execute("DELETE FROM scratchpads WHERE session_id = ?;", session_id));
  RETURN_IF_ERROR(Execute("DELETE FROM sessions WHERE id = ?;", session_id));
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
      "INSERT INTO sessions (id, accordion_retain_groups, accordion_watermark_tokens, "
      "accordion_epoch_start_group_id, active_skills) "
      "SELECT ?, accordion_retain_groups, accordion_watermark_tokens, accordion_epoch_start_group_id, active_skills FROM sessions "
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
      "INSERT INTO usage (session_id, model, prompt_tokens, completion_tokens, total_tokens, cached_prompt_tokens, "
      "cache_write_prompt_tokens, created_at) "
      "SELECT ?, model, prompt_tokens, completion_tokens, total_tokens, cached_prompt_tokens, "
      "cache_write_prompt_tokens, created_at FROM usage WHERE session_id = ?;",
      {target_id, source_id});
  if (!status.ok()) return rollback_on_failure(status);
  status = Execute(
      "INSERT INTO scratchpads (session_id, content, updated_at) "
      "SELECT ?, content, updated_at FROM scratchpads WHERE session_id = ? "
      "ON CONFLICT(session_id) DO UPDATE SET content = excluded.content, updated_at = excluded.updated_at;",
      {target_id, source_id});
  if (!status.ok()) return rollback_on_failure(status);
  return Execute("COMMIT;");
}

absl::Status Database::CloneSessionThroughGroup(const std::string& source_id, const std::string& target_id,
                                                const std::string& group_id) {
  RETURN_IF_ERROR(SessionExists(this, source_id));
  RETURN_IF_ERROR(SessionDoesNotExist(this, target_id));
  ASSIGN_OR_RETURN(Database::Message cutoff, LastMessageForGroup(this, source_id, group_id));

  RETURN_IF_ERROR(Execute("BEGIN TRANSACTION;"));
  absl::Status status = Execute(
      "INSERT INTO sessions (id, accordion_retain_groups, accordion_watermark_tokens, "
      "accordion_epoch_start_group_id, active_skills) "
      "SELECT ?, accordion_retain_groups, accordion_watermark_tokens, accordion_epoch_start_group_id, active_skills "
      "FROM sessions WHERE id = ?;",
      {target_id, source_id});
  if (!status.ok()) return RollbackTransaction(this, status);
  status = Execute(
      "INSERT INTO messages (session_id, role, content, tool_call_id, status, created_at, group_id, parsing_strategy, tokens) "
      "SELECT ?, role, content, tool_call_id, status, created_at, group_id, parsing_strategy, tokens "
      "FROM messages WHERE session_id = ? AND status != 'dropped' AND (created_at < ? OR (created_at = ? AND id <= ?)) "
      "ORDER BY created_at ASC, id ASC;",
      {target_id, source_id, cutoff.created_at, cutoff.created_at, std::to_string(cutoff.id)});
  if (!status.ok()) return RollbackTransaction(this, status);
  status = Execute(
      "INSERT INTO usage (session_id, model, prompt_tokens, completion_tokens, total_tokens, cached_prompt_tokens, "
      "cache_write_prompt_tokens, created_at) "
      "SELECT ?, model, prompt_tokens, completion_tokens, total_tokens, cached_prompt_tokens, "
      "cache_write_prompt_tokens, created_at FROM usage WHERE session_id = ? AND created_at <= ?;",
      {target_id, source_id, cutoff.created_at});
  if (!status.ok()) return RollbackTransaction(this, status);
  status = Execute(
      "INSERT INTO scratchpads (session_id, content, updated_at) SELECT ?, content, updated_at FROM scratchpads "
      "WHERE session_id = ? ON CONFLICT(session_id) DO UPDATE SET content = excluded.content, "
      "updated_at = excluded.updated_at;",
      {target_id, source_id});
  if (!status.ok()) return RollbackTransaction(this, status);
  return Execute("COMMIT;");
}

absl::Status Database::RollbackSessionToGroup(const std::string& session_id, const std::string& group_id) {
  RETURN_IF_ERROR(SessionExists(this, session_id));
  ASSIGN_OR_RETURN(Database::Message cutoff, LastMessageForGroup(this, session_id, group_id));

  RETURN_IF_ERROR(Execute("BEGIN TRANSACTION;"));
  absl::Status status = Execute("UPDATE messages SET status = 'dropped' WHERE session_id = ? "
                                "AND (created_at > ? OR (created_at = ? AND id > ?));",
                                {session_id, cutoff.created_at, cutoff.created_at, std::to_string(cutoff.id)});
  if (!status.ok()) return RollbackTransaction(this, status);
  status = Execute("DELETE FROM usage WHERE session_id = ? AND created_at > ?;", {session_id, cutoff.created_at});
  if (!status.ok()) return RollbackTransaction(this, status);
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
