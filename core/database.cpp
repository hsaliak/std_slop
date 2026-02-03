#include "core/database.h"

#include <iostream>

#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"
#include "absl/strings/substitute.h"

#include "core/status_macros.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>
namespace slop {

absl::Status Database::Statement::Prepare() {
  sqlite3_stmt* raw_stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql_.c_str(), -1, &raw_stmt, nullptr);
  if (rc != SQLITE_OK) {
    std::string err = sqlite3_errmsg(db_);
    LOG(ERROR) << "Prepare error: " << err << " (SQL: " << sql_ << ")";
    return absl::InternalError("Prepare error: " + err + " (SQL: " + sql_ + ")");
  }
  stmt_.reset(raw_stmt);
  return absl::OkStatus();
}

absl::Status Database::Statement::BindInt(int index, int value) {
  if (sqlite3_bind_int(stmt_.get(), index, value) != SQLITE_OK) {
    return absl::InternalError("BindInt error: " + std::string(sqlite3_errmsg(db_)));
  }
  return absl::OkStatus();
}

absl::Status Database::Statement::BindInt64(int index, int64_t value) {
  if (sqlite3_bind_int64(stmt_.get(), index, value) != SQLITE_OK) {
    return absl::InternalError("BindInt64 error: " + std::string(sqlite3_errmsg(db_)));
  }
  return absl::OkStatus();
}

absl::Status Database::Statement::BindDouble(int index, double value) {
  if (sqlite3_bind_double(stmt_.get(), index, value) != SQLITE_OK) {
    return absl::InternalError("BindDouble error: " + std::string(sqlite3_errmsg(db_)));
  }
  return absl::OkStatus();
}

absl::Status Database::Statement::BindText(int index, const std::string& value) {
  if (sqlite3_bind_text(stmt_.get(), index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    return absl::InternalError("BindText error: " + std::string(sqlite3_errmsg(db_)));
  }
  return absl::OkStatus();
}

absl::Status Database::Statement::BindNull(int index) {
  if (sqlite3_bind_null(stmt_.get(), index) != SQLITE_OK) {
    return absl::InternalError("BindNull error: " + std::string(sqlite3_errmsg(db_)));
  }
  return absl::OkStatus();
}

absl::StatusOr<bool> Database::Statement::Step() {
  int rc = sqlite3_step(stmt_.get());
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

int Database::Statement::ColumnInt(int index) { return sqlite3_column_int(stmt_.get(), index); }

int64_t Database::Statement::ColumnInt64(int index) { return sqlite3_column_int64(stmt_.get(), index); }

double Database::Statement::ColumnDouble(int index) { return sqlite3_column_double(stmt_.get(), index); }

std::string Database::Statement::ColumnText(int index) {
  const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_.get(), index));
  return text ? std::string(text) : "";
}

int Database::Statement::ColumnType(int index) { return sqlite3_column_type(stmt_.get(), index); }

const char* Database::Statement::ColumnName(int index) { return sqlite3_column_name(stmt_.get(), index); }

int Database::Statement::ColumnCount() { return sqlite3_column_count(stmt_.get()); }

bool Database::IsStopWord(const std::string& word) {
  static const absl::flat_hash_set<std::string> kStopWords = {
      "about", "above", "after",   "again", "against", "all",   "and",    "any",   "because",  "been",      "before",
      "being", "below", "between", "both",  "but",     "could", "did",    "does",  "doing",    "down",      "during",
      "each",  "few",   "for",     "from",  "further", "had",   "has",    "have",  "having",   "here",      "how",
      "into",  "its",   "just",    "more",  "most",    "now",   "off",    "once",  "only",     "other",     "ought",
      "our",   "ours",  "out",     "own",   "same",    "she",   "should", "some",  "such",     "than",      "that",
      "the",   "their", "theirs",  "them",  "then",    "there", "these",  "they",  "this",     "those",     "through",
      "too",   "under", "until",   "very",  "was",     "were",  "what",   "when",  "where",    "which",     "while",
      "who",   "whom",  "why",     "with",  "would",   "you",   "your",   "yours", "yourself", "yourselves"};
  return kStopWords.find(word) != kStopWords.end();
}

std::vector<std::string> Database::ExtractTags(const std::string& text) {
  std::vector<std::string> words = absl::StrSplit(text, absl::ByAnyChar(" \t\n\r.,;:()[]{}<>\"'-"));
  std::vector<std::string> tags;
  absl::flat_hash_set<std::string> seen;
  for (const auto& w : words) {
    std::string word = absl::AsciiStrToLower(absl::StripAsciiWhitespace(w));
    if (word.length() > 3 && !IsStopWord(word)) {
      if (seen.find(word) == seen.end()) {
        tags.push_back(word);
        seen.insert(word);
      }
    }
  }
  return tags;
}

absl::StatusOr<std::unique_ptr<Database::Statement>> Database::Prepare(const std::string& sql) {
  absl::MutexLock lock(&mu_);
  auto stmt = std::make_unique<Statement>(db_.get(), sql);
  auto status = stmt->Prepare();
  if (!status.ok()) return status;
  return stmt;
}

absl::Status Database::Init(const std::string& db_path) {
  LOG(INFO) << "Initializing database at " << db_path;
  sqlite3* raw_db = nullptr;
  int rc = sqlite3_open(db_path.c_str(), &raw_db);
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
        name TEXT,
        context_size INTEGER DEFAULT 2,
        scratchpad TEXT,
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

    CREATE TABLE IF NOT EXISTS llm_memos (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        content TEXT NOT NULL,
        semantic_tags TEXT NOT NULL,
        created_at DATETIME DEFAULT CURRENT_TIMESTAMP
    );
  )";

  rc = sqlite3_exec(raw_db, schema, nullptr, nullptr, nullptr);
  if (rc != SQLITE_OK) {
    std::string err = sqlite3_errmsg(raw_db);
    sqlite3_close(raw_db);
    return absl::InternalError("Schema error: " + err);
  }

  // Migration: Add tokens column to messages table if it doesn't exist
  (void)sqlite3_exec(raw_db, "ALTER TABLE messages ADD COLUMN tokens INTEGER DEFAULT 0;", nullptr, nullptr, nullptr);
  (void)sqlite3_exec(raw_db, "ALTER TABLE skills ADD COLUMN activation_count INTEGER DEFAULT 0;", nullptr, nullptr,
                     nullptr);
  (void)sqlite3_exec(raw_db, "ALTER TABLE sessions ADD COLUMN active_skills TEXT;", nullptr, nullptr, nullptr);
  (void)sqlite3_exec(raw_db, "ALTER TABLE tools ADD COLUMN call_count INTEGER DEFAULT 0;", nullptr, nullptr, nullptr);

  {
    absl::MutexLock lock(&mu_);
    db_.reset(raw_db);
  }

  absl::Status s = RegisterDefaultTools();
  if (!s.ok()) return s;

  s = RegisterDefaultSkills();
  if (!s.ok()) return s;

  return absl::OkStatus();
}

absl::Status Database::RegisterDefaultTools() {
  std::vector<Tool> default_tools = {
      {"read_file", "Read the content of a file from the local filesystem.",
       R"({"type":"object","properties":{"path":{"type":"string"},"start_line":{"type":"integer"},"end_line":{"type":"integer"}},"required":["path"]})",
       true},
      {"write_file", "Write content to a file in the local filesystem.",
       R"({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]})",
       true},
      {"execute_bash", "Execute a bash command on the local system.",
       R"({"type":"object","properties":{"command":{"type":"string"}},"required":["command"]})", true},
      {"grep_tool",
       "Search for a pattern in the codebase using grep. Delegates to git_grep_tool if available in a git repository. "
       "If not in a git repository, it is highly recommended to initialize one with 'git init' for better performance "
       "and feature support.",
       R"({"type":"object","properties":{"pattern":{"type":"string"},"path":{"type":"string"},"context":{"type":"integer"}},"required":["pattern"]})",
       true},
      {"git_grep_tool",
       "Comprehensive search using git grep. Optimized for git repositories, honors .gitignore, and can search "
       "history.",
       R"({"type":"object","properties":{"pattern":{"type":"string"},"path":{"type":"string"},"case_insensitive":{"type":"boolean"},"word_regexp":{"type":"boolean"},"line_number":{"type":"boolean","default":true},"count":{"type":"boolean"},"before":{"type":"integer"},"after":{"type":"integer"},"context":{"type":"integer"},"files_with_matches":{"type":"boolean"},"all_match":{"type":"boolean"},"pcre":{"type":"boolean"},"show_function":{"type":"boolean"},"function_context":{"type":"boolean"},"cached":{"type":"boolean"},"branch":{"type":"string"}},"required":["pattern"]})",
       true},
      {"query_db", "Query the local SQLite database using SQL.",
       R"({"type":"object","properties":{"sql":{"type":"string"}},"required":["sql"]})", true},
      {"apply_patch", "Applies partial changes to a file by matching a specific block of text and replacing it.",
       R"({"type":"object","properties":{"path":{"type":"string"},"patches":{"type":"array","items":{"type":"object","properties":{"find":{"type":"string"},"replace":{"type":"string"}},"required":["find","replace"]}}},"required":["path","patches"]})",
       true},
      {"save_memo", "Save a memo with semantic tags for later retrieval.",
       R"({"type":"object","properties":{"content":{"type":"string"},"tags":{"type":"array","items":{"type":"string"}}},"required":["content","tags"]})",
       true},
      {"retrieve_memos", "Retrieve memos based on semantic tags.",
       R"({"type":"object","properties":{"tags":{"type":"array","items":{"type":"string"}}},"required":["tags"]})",
       true},
      {"list_directory", "List files and directories with optional depth and git awareness.",
       R"({"type":"object","properties":{"path":{"type":"string"},"depth":{"type":"integer"},"git_only":{"type":"boolean"}},"required":[]})",
       true},
      {"manage_scratchpad", "Manage a persistent markdown scratchpad for the current session.",
       R"({"type":"object","properties":{"action":{"type":"string","enum":["read","update","append"]},"content":{"type":"string"}},"required":["action"]})",
       true},
      {"describe_db", "Describe the database schema and tables.", R"({"type":"object","properties":{}})", true},
      {"use_skill", "Activate or deactivate a specialized skill/persona.",
       R"({"type":"object","properties":{"name":{"type":"string"},"action":{"type":"string","enum":["activate","deactivate"],"default":"activate"}},"required":["name"]})",
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
       "You only plan. You _do_ _not_ implement anything, and do not write or modify any files. You give me ideas to "
       "plan ONLY!"},
      {0, "dba", "Database Administrator specializing in SQLite schema design, optimization, and data integrity.",
       "As a DBA, you are the steward of the project's data. You focus on efficient schema design, precise query "
       "construction, and maintaining data integrity. When interacting with the database: 1. Always verify schema "
       "before operations. 2. Use transactions for complex updates. 3. Provide clear explanations for schema changes. "
       "4. Optimize for performance while ensuring clarity."},
      {0, "c++_expert",
       "Enforces strict adherence to project C++ constraints: C++17, Google Style, no exceptions, RAII/unique_ptr, "
       "absl::Status.",
       "You are a C++ Expert specialized in the std::slop codebase.\nYou MUST adhere to these constraints in every "
       "code change:\n- Language: C++17.\n- Style: Google C++ Style Guide.\n- Exceptions: Strictly disabled "
       "(-fno-exceptions). Never use try, catch, or throw.\n- Memory: Use RAII and std::unique_ptr exclusively. Avoid "
       "raw new/delete. Use stack allocation where possible.\n- Error Handling: Use absl::Status and absl::StatusOr "
       "for all fallible operations.\n- Threading: Avoid threading and async primitives. If necessary, use absl based "
       "primitives with std::thread and provide tsan tests.\n- Design: Prefer simple, readable code over complex "
       "template metaprogramming or deep inheritance.\nYou ALWAYS run all tests. You ALWAYS ensure affected targets "
       "compile."},
      {0, "code_reviewer",
       "Multilingual code reviewer enforcing language-specific standards (Google C++, PEP8, etc.) and project "
       "conventions.",
       "You are a strict code reviewer. Your goal is to review code changes against industry-standard style guides and "
       "project conventions.\nStandards to follow:\n- C++: Google C++ Style Guide.\n- Python: PEP 8.\n- Others: "
       "Appropriate de-facto industry standards (e.g., Effective Java, Airbnb JS Style Guide).\nYou do NOT implement "
       "changes. You ONLY provide an annotated set of required changes or comments. Only after explicit user approval "
       "can you proceed with addressing the issues identified. Focus on style, safety, and readability. For new files, "
       "use `git add --intent-to-add` before `git diff`. Always list the files reviewed in your summary."}};

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
    auto j = nlohmann::json::parse(active_skills_raw, nullptr, false);
    if (!j.is_discarded() && j.is_array()) {
      return j.get<std::vector<std::string>>();
    }
  }
  return std::vector<std::string>();
}

absl::Status Database::SetContextWindow(const std::string& session_id, int size) {
  return Execute("INSERT OR REPLACE INTO sessions (id, context_size) VALUES (?, ?);", session_id, size);
}

absl::StatusOr<Database::ContextSettings> Database::GetContextSettings(const std::string& session_id) {
  std::string sql = "SELECT context_size FROM sessions WHERE id = ?";
  ASSIGN_OR_RETURN(auto stmt, Prepare(sql));

  RETURN_IF_ERROR(stmt->BindText(1, session_id));

  auto row_or = stmt->Step();
  if (!row_or.ok()) return row_or.status();

  ContextSettings settings = {2};  // Default
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
      "INSERT INTO sessions (id, name, context_size, scratchpad, active_skills) "
      "SELECT ?, name, context_size, scratchpad, active_skills FROM sessions "
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

absl::Status Database::AddMemo(const std::string& content, const std::string& semantic_tags) {
  auto stmt_or = Prepare("INSERT INTO llm_memos (content, semantic_tags) VALUES (?, ?)");
  if (!stmt_or.ok()) return stmt_or.status();
  auto& stmt = *stmt_or;
  (void)stmt->BindText(1, content);
  (void)stmt->BindText(2, semantic_tags);
  return stmt->Run();
}

absl::Status Database::UpdateMemo(int id, const std::string& content, const std::string& semantic_tags) {
  auto stmt_or = Prepare("UPDATE llm_memos SET content = ?, semantic_tags = ? WHERE id = ?");
  if (!stmt_or.ok()) return stmt_or.status();
  auto& stmt = *stmt_or;
  (void)stmt->BindText(1, content);
  (void)stmt->BindText(2, semantic_tags);
  (void)stmt->BindInt(3, id);
  return stmt->Run();
}

absl::Status Database::DeleteMemo(int id) {
  auto stmt_or = Prepare("DELETE FROM llm_memos WHERE id = ?");
  if (!stmt_or.ok()) return stmt_or.status();
  auto& stmt = *stmt_or;
  (void)stmt->BindInt(1, id);
  return stmt->Run();
}

absl::StatusOr<Database::Memo> Database::GetMemo(int id) {
  auto stmt_or = Prepare("SELECT id, content, semantic_tags, created_at FROM llm_memos WHERE id = ?");
  if (!stmt_or.ok()) return stmt_or.status();
  auto& stmt = *stmt_or;
  (void)stmt->BindInt(1, id);

  auto res = stmt->Step();
  if (!res.ok()) return res.status();
  if (!*res) return absl::NotFoundError(absl::Substitute("Memo $0 not found", id));

  return Memo{
      stmt->ColumnInt(0),
      stmt->ColumnText(1),
      stmt->ColumnText(2),
      stmt->ColumnText(3),
  };
}

absl::StatusOr<std::vector<Database::Memo>> Database::GetMemosByTags(const std::vector<std::string>& tags_input) {
  if (tags_input.empty()) return std::vector<Memo>();

  std::set<std::string> unique_tags;
  for (const auto& t : tags_input) {
    auto extracted = ExtractTags(t);
    unique_tags.insert(extracted.begin(), extracted.end());
    // Also add the raw tag if it's not a stopword and long enough
    std::string lower_t = absl::AsciiStrToLower(absl::StripAsciiWhitespace(t));
    if (lower_t.length() > 2 && !IsStopWord(lower_t)) {
      unique_tags.insert(lower_t);
    }
  }

  if (unique_tags.empty()) return std::vector<Memo>();
  std::vector<std::string> tags(unique_tags.begin(), unique_tags.end());

  std::string sql =
      "SELECT DISTINCT m.id, m.content, m.semantic_tags, m.created_at "
      "FROM llm_memos m, json_each(m.semantic_tags) j "
      "WHERE ";
  for (size_t i = 0; i < tags.size(); ++i) {
    sql += "(j.value = ? OR j.value LIKE ? OR j.value LIKE ? OR j.value LIKE ?)";
    if (i < tags.size() - 1) sql += " OR ";
  }

  auto stmt_or = Prepare(sql);
  if (!stmt_or.ok()) return stmt_or.status();
  auto& stmt = *stmt_or;
  for (size_t i = 0; i < tags.size(); ++i) {
    int base = i * 4 + 1;
    (void)stmt->BindText(base, tags[i]);                    // Exact
    (void)stmt->BindText(base + 1, tags[i] + "-%");         // Prefix: arch-
    (void)stmt->BindText(base + 2, "%-" + tags[i]);         // Suffix: -arch
    (void)stmt->BindText(base + 3, "%-" + tags[i] + "-%");  // Middle: -arch-
  }

  std::vector<Memo> results;
  while (true) {
    auto res = stmt->Step();
    if (!res.ok()) return res.status();
    if (!*res) break;

    results.push_back({
        stmt->ColumnInt(0),
        stmt->ColumnText(1),
        stmt->ColumnText(2),
        stmt->ColumnText(3),
    });
  }
  return results;
}

absl::StatusOr<std::vector<Database::Memo>> Database::GetAllMemos() {
  auto stmt_or = Prepare("SELECT id, content, semantic_tags, created_at FROM llm_memos");
  if (!stmt_or.ok()) return stmt_or.status();
  auto& stmt = *stmt_or;

  std::vector<Memo> results;
  while (true) {
    auto res = stmt->Step();
    if (!res.ok()) return res.status();
    if (!*res) break;

    results.push_back({
        stmt->ColumnInt(0),
        stmt->ColumnText(1),
        stmt->ColumnText(2),
        stmt->ColumnText(3),
    });
  }
  return results;
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
  return results.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

absl::Status Database::UpdateScratchpad(const std::string& session_id, const std::string& scratchpad) {
  auto stmt_or = Prepare(
      "INSERT INTO sessions (id, scratchpad) VALUES (?, ?) "
      "ON CONFLICT(id) DO UPDATE SET scratchpad=excluded.scratchpad");
  if (!stmt_or.ok()) return stmt_or.status();
  auto stmt = std::move(*stmt_or);
  (void)stmt->BindText(1, session_id);
  (void)stmt->BindText(2, scratchpad);
  return stmt->Run();
}

absl::StatusOr<std::string> Database::GetScratchpad(const std::string& session_id) {
  auto stmt_or = Prepare("SELECT scratchpad FROM sessions WHERE id = ?");
  if (!stmt_or.ok()) return stmt_or.status();
  auto stmt = std::move(*stmt_or);
  (void)stmt->BindText(1, session_id);
  auto res = stmt->Step();
  if (!res.ok()) return res.status();
  if (!*res) return "";  // Return empty string if session not found
  return stmt->ColumnText(0);
}

}  // namespace slop
