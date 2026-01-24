#include "database.h"

#include <iostream>
#include <unordered_set>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"
#include "absl/strings/substitute.h"

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
  return absl::InternalError("Step error: " + std::string(sqlite3_errmsg(db_)));
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
  static const std::unordered_set<std::string> kStopWords = {
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
  std::set<std::string> seen;
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
  auto stmt = std::make_unique<Statement>(db_.get(), sql);
  auto status = stmt->Prepare();
  if (!status.ok()) return status;
  return stmt;
}

absl::Status Database::Init(const std::string& db_path) {
  LOG(INFO) << "Initializing database at " << db_path;
  sqlite3* raw_db = nullptr;
  int rc = sqlite3_open(db_path.c_str(), &raw_db);
  db_.reset(raw_db);
  if (rc != SQLITE_OK) {
    std::string err = sqlite3_errmsg(db_.get());
    LOG(ERROR) << "Failed to open database: " << err;
    return absl::InternalError("Failed to open database: " + err);
  }

  sqlite3_exec(db_.get(), "DROP TABLE IF EXISTS code_search;", nullptr, nullptr, nullptr);

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
        parsing_strategy TEXT
    );

    CREATE TABLE IF NOT EXISTS tools (
        name TEXT PRIMARY KEY,
        description TEXT,
        json_schema TEXT,
        is_enabled INTEGER DEFAULT 1
    );

    CREATE TABLE IF NOT EXISTS skills (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT UNIQUE,
        description TEXT,
        system_prompt_patch TEXT
    );

    CREATE TABLE IF NOT EXISTS sessions (
        id TEXT PRIMARY KEY,
        context_size INTEGER DEFAULT 5
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

    CREATE TABLE IF NOT EXISTS todos (
        id INTEGER NOT NULL,
        group_name TEXT,
        description TEXT,
        status TEXT CHECK(status IN ('Open', 'Complete')) DEFAULT 'Open',
        PRIMARY KEY (id, group_name)
    );

    CREATE TABLE IF NOT EXISTS llm_memos (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        content TEXT NOT NULL,
        semantic_tags TEXT NOT NULL,
        created_at DATETIME DEFAULT CURRENT_TIMESTAMP
    );
  )";

  rc = sqlite3_exec(db_.get(), schema, nullptr, nullptr, nullptr);
  if (rc != SQLITE_OK) {
    return absl::InternalError("Schema error: " + std::string(sqlite3_errmsg(db_.get())));
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
       "Returns matching lines with context.",
       R"({"type":"object","properties":{"pattern":{"type":"string"},"path":{"type":"string"},"context":{"type":"integer"}},"required":["pattern"]})",
       true},
      {"git_grep_tool",
       "Comprehensive search using git grep. Optimized for git repositories, honors .gitignore, and can search "
       "history.",
       R"({"type":"object","properties":{"pattern":{"type":"string"},"patterns":{"type":"array","items":{"type":"string"}},"path":{"type":"string"},"case_insensitive":{"type":"boolean"},"word_regexp":{"type":"boolean"},"line_number":{"type":"boolean","default":true},"count":{"type":"boolean"},"before":{"type":"integer"},"after":{"type":"integer"},"context":{"type":"integer"},"files_with_matches":{"type":"boolean"},"all_match":{"type":"boolean"},"pcre":{"type":"boolean"},"show_function":{"type":"boolean"},"cached":{"type":"boolean"},"branch":{"type":"string"}},"required":["pattern"]})",
       true},
      {"search_code", "Search for code snippets in the codebase using grep.",
       R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"]})", true},
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
       true}};

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
      {0, "todo_processor",
       "Reads open todos from the database and executes them sequentially after user confirmation.",
       "You are now in Todo Processing mode. Your task is to fetch the next 'Open' todo from the 'todos' table "
       "(ordered by id) for the specified group. Once fetched, treat its description as your next goal. Plan the "
       "implementation, present it to the user, and wait for approval. After successful completion, update the todo's "
       "status to 'Complete' and proceed to the next 'Open' todo.\n"}};

  for (const auto& s : default_skills) {
    absl::Status status = RegisterSkill(s);
    if (!status.ok()) return status;
  }
  return absl::OkStatus();
}

absl::Status Database::Execute(const std::string& sql) {
  char* errmsg = nullptr;
  int rc = sqlite3_exec(db_.get(), sql.c_str(), nullptr, nullptr, &errmsg);
  if (rc != SQLITE_OK) {
    std::string err = errmsg ? errmsg : "unknown error";
    sqlite3_free(errmsg);
    return absl::InternalError("Execute error: " + err);
  }
  return absl::OkStatus();
}

absl::Status Database::AppendMessage(const std::string& session_id, const std::string& role, const std::string& content,
                                     const std::string& tool_call_id, const std::string& status,
                                     const std::string& group_id, const std::string& parsing_strategy) {
  std::string sql =
      "INSERT INTO messages (session_id, role, content, tool_call_id, status, group_id, parsing_strategy) VALUES (?, "
      "?, ?, ?, ?, ?, ?)";
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

  (void)stmt->BindText(1, session_id);
  (void)stmt->BindText(2, role);
  (void)stmt->BindText(3, content);
  if (tool_call_id.empty()) {
    (void)stmt->BindNull(4);
  } else {
    (void)stmt->BindText(4, tool_call_id);
  }
  (void)stmt->BindText(5, status);
  if (group_id.empty()) {
    (void)stmt->BindNull(6);
  } else {
    (void)stmt->BindText(6, group_id);
  }
  if (parsing_strategy.empty()) {
    (void)stmt->BindNull(7);
  } else {
    (void)stmt->BindText(7, parsing_strategy);
  }

  return stmt->Run();
}

absl::Status Database::UpdateMessageStatus(int id, const std::string& status) {
  std::string sql = "UPDATE messages SET status = ? WHERE id = ?";
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

  (void)stmt->BindText(1, status);
  (void)stmt->BindInt(2, id);

  return stmt->Run();
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
    sql = absl::Substitute(
        "SELECT id, session_id, role, content, tool_call_id, status, created_at, group_id, parsing_strategy "
        "FROM messages WHERE session_id = ? $0 "
        "AND (group_id IS NULL OR group_id IN (SELECT DISTINCT group_id FROM messages WHERE session_id = ? AND "
        "group_id IS NOT NULL $0 ORDER BY created_at DESC, id DESC LIMIT ?)) "
        "ORDER BY created_at ASC, id ASC",
        drop_filter);
  } else {
    sql = absl::Substitute(
        "SELECT id, session_id, role, content, tool_call_id, status, created_at, group_id, parsing_strategy "
        "FROM messages WHERE session_id = ? $0 "
        "ORDER BY created_at ASC, id ASC",
        drop_filter);
  }

  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

  (void)stmt->BindText(1, session_id);
  if (window_size > 0) {
    (void)stmt->BindText(2, session_id);
    (void)stmt->BindInt(3, window_size);
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
      "SELECT id, session_id, role, content, tool_call_id, status, created_at, group_id, parsing_strategy FROM "
      "messages WHERE group_id IN (" +
      placeholders + ") ORDER BY created_at ASC, id ASC";

  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

  for (size_t i = 0; i < group_ids.size(); ++i) {
    (void)stmt->BindText(i + 1, group_ids[i]);
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
    messages.push_back(m);
  }
  return messages;
}

absl::StatusOr<std::string> Database::GetLastGroupId(const std::string& session_id) {
  std::string sql =
      "SELECT group_id FROM messages WHERE session_id = ? AND group_id IS NOT NULL ORDER BY created_at DESC, id DESC "
      "LIMIT 1";
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;
  (void)stmt->BindText(1, session_id);
  auto row_or = stmt->Step();
  if (!row_or.ok()) return row_or.status();
  if (*row_or) {
    return stmt->ColumnText(0);
  }
  return absl::NotFoundError("No group found");
}

absl::Status Database::RecordUsage(const std::string& session_id, const std::string& model, int prompt_tokens,
                                   int completion_tokens) {
  std::string sql =
      "INSERT INTO usage (session_id, model, prompt_tokens, completion_tokens, total_tokens) VALUES (?, ?, ?, ?, ?)";
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

  (void)stmt->BindText(1, session_id);
  (void)stmt->BindText(2, model);
  (void)stmt->BindInt(3, prompt_tokens);
  (void)stmt->BindInt(4, completion_tokens);
  (void)stmt->BindInt(5, prompt_tokens + completion_tokens);

  return stmt->Run();
}

absl::StatusOr<Database::TotalUsage> Database::GetTotalUsage(const std::string& session_id) {
  std::string sql = "SELECT SUM(prompt_tokens), SUM(completion_tokens), SUM(total_tokens) FROM usage";
  if (!session_id.empty()) {
    sql += " WHERE session_id = ?";
  }

  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

  if (!session_id.empty()) {
    (void)stmt->BindText(1, session_id);
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
  std::string sql = "INSERT OR REPLACE INTO tools (name, description, json_schema, is_enabled) VALUES (?, ?, ?, ?)";
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

  (void)stmt->BindText(1, tool.name);
  (void)stmt->BindText(2, tool.description);
  (void)stmt->BindText(3, tool.json_schema);
  (void)stmt->BindInt(4, tool.is_enabled ? 1 : 0);

  return stmt->Run();
}

absl::StatusOr<std::vector<Database::Tool>> Database::GetEnabledTools() {
  std::string sql = "SELECT name, description, json_schema, is_enabled FROM tools WHERE is_enabled = 1";
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

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
    tools.push_back(t);
  }
  return tools;
}

absl::Status Database::RegisterSkill(const Skill& skill) {
  std::string sql = "INSERT OR IGNORE INTO skills (name, description, system_prompt_patch) VALUES (?, ?, ?)";
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

  (void)stmt->BindText(1, skill.name);
  (void)stmt->BindText(2, skill.description);
  (void)stmt->BindText(3, skill.system_prompt_patch);

  return stmt->Run();
}

absl::Status Database::UpdateSkill(const Skill& skill) {
  std::string sql = "UPDATE skills SET description = ?, system_prompt_patch = ? WHERE name = ?";
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

  (void)stmt->BindText(1, skill.description);
  (void)stmt->BindText(2, skill.system_prompt_patch);
  (void)stmt->BindText(3, skill.name);

  return stmt->Run();
}

absl::Status Database::DeleteSkill(const std::string& name_or_id) {
  std::string sql = "DELETE FROM skills WHERE name = ? OR id = ?";
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

  (void)stmt->BindText(1, name_or_id);
  int id = 0;
  if (absl::SimpleAtoi(name_or_id, &id)) {
    (void)stmt->BindInt(2, id);
  } else {
    (void)stmt->BindNull(2);
  }

  return stmt->Run();
}

absl::StatusOr<std::vector<Database::Skill>> Database::GetSkills() {
  std::string sql = "SELECT id, name, description, system_prompt_patch FROM skills";
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

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
    skills.push_back(s);
  }
  return skills;
}

absl::Status Database::SetContextWindow(const std::string& session_id, int size) {
  std::string sql = "INSERT OR REPLACE INTO sessions (id, context_size) VALUES (?, ?)";
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

  (void)stmt->BindText(1, session_id);
  (void)stmt->BindInt(2, size);

  return stmt->Run();
}

absl::StatusOr<Database::ContextSettings> Database::GetContextSettings(const std::string& session_id) {
  std::string sql = "SELECT context_size FROM sessions WHERE id = ?";
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

  (void)stmt->BindText(1, session_id);

  auto row_or = stmt->Step();
  if (!row_or.ok()) return row_or.status();

  ContextSettings settings = {5};  // Default
  if (*row_or) {
    settings.size = stmt->ColumnInt(0);
  }
  return settings;
}

absl::Status Database::SetSessionState(const std::string& session_id, const std::string& state_blob) {
  std::string sql = "INSERT OR REPLACE INTO session_state (session_id, state_blob) VALUES (?, ?)";
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

  (void)stmt->BindText(1, session_id);
  (void)stmt->BindText(2, state_blob);

  return stmt->Run();
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
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

  (void)stmt->BindText(1, session_id);

  auto row_or = stmt->Step();
  if (!row_or.ok()) return row_or.status();

  if (*row_or) {
    return stmt->ColumnText(0);
  }
  return absl::NotFoundError("Session state not found");
}

absl::Status Database::DeleteSession(const std::string& session_id) {
  // 1. Delete messages
  {
    std::string sql = "DELETE FROM messages WHERE session_id = ?";
    auto stmt_or = Prepare(sql);
    CHECK_OK(stmt_or.status());
    (void)(*stmt_or)->BindText(1, session_id);
    auto status = (*stmt_or)->Run();
    if (!status.ok()) return status;
  }
  // 2. Delete usage
  {
    std::string sql = "DELETE FROM usage WHERE session_id = ?";
    auto stmt_or = Prepare(sql);
    CHECK_OK(stmt_or.status());
    (void)(*stmt_or)->BindText(1, session_id);
    auto status = (*stmt_or)->Run();
    if (!status.ok()) return status;
  }
  // 3. Delete session settings
  {
    std::string sql = "DELETE FROM sessions WHERE id = ?";
    auto stmt_or = Prepare(sql);
    CHECK_OK(stmt_or.status());
    (void)(*stmt_or)->BindText(1, session_id);
    auto status = (*stmt_or)->Run();
    if (!status.ok()) return status;
  }
  // 4. Delete session state
  {
    std::string sql = "DELETE FROM session_state WHERE session_id = ?";
    auto stmt_or = Prepare(sql);
    CHECK_OK(stmt_or.status());
    (void)(*stmt_or)->BindText(1, session_id);
    auto status = (*stmt_or)->Run();
    if (!status.ok()) return status;
  }
  return absl::OkStatus();
}

absl::Status Database::AddTodo(const std::string& group_name, const std::string& description) {
  std::string sql_max = "SELECT COALESCE(MAX(id), 0) FROM todos WHERE group_name = ?";
  auto stmt_max_or = Prepare(sql_max);
  CHECK_OK(stmt_max_or.status());
  (void)(*stmt_max_or)->BindText(1, group_name);
  auto row_or = (*stmt_max_or)->Step();
  if (!row_or.ok()) return row_or.status();
  int next_id = (*stmt_max_or)->ColumnInt(0) + 1;

  std::string sql = "INSERT INTO todos (id, group_name, description) VALUES (?, ?, ?)";
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

  (void)stmt->BindInt(1, next_id);
  (void)stmt->BindText(2, group_name);
  (void)stmt->BindText(3, description);

  return stmt->Run();
}

absl::StatusOr<std::vector<Database::Todo>> Database::GetTodos(const std::string& group_name) {
  std::string sql;
  if (group_name.empty()) {
    sql = "SELECT id, group_name, description, status FROM todos ORDER BY group_name ASC, id ASC";
  } else {
    sql = "SELECT id, group_name, description, status FROM todos WHERE group_name = ? ORDER BY id ASC";
  }
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

  if (!group_name.empty()) {
    (void)stmt->BindText(1, group_name);
  }

  std::vector<Todo> todos;
  while (true) {
    auto row_or = stmt->Step();
    if (!row_or.ok()) return row_or.status();
    if (!*row_or) break;

    Todo t;
    t.id = stmt->ColumnInt(0);
    t.group_name = stmt->ColumnText(1);
    t.description = stmt->ColumnText(2);
    t.status = stmt->ColumnText(3);
    todos.push_back(t);
  }
  return todos;
}

absl::Status Database::UpdateTodo(int id, const std::string& group_name, const std::string& description) {
  std::string sql = "UPDATE todos SET description = ? WHERE group_name = ? AND id = ?";
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

  (void)stmt->BindText(1, description);
  (void)stmt->BindText(2, group_name);
  (void)stmt->BindInt(3, id);

  return stmt->Run();
}

absl::Status Database::UpdateTodoStatus(int id, const std::string& group_name, const std::string& status) {
  std::string sql = "UPDATE todos SET status = ? WHERE group_name = ? AND id = ?";
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

  (void)stmt->BindText(1, status);
  (void)stmt->BindText(2, group_name);
  (void)stmt->BindInt(3, id);

  return stmt->Run();
}

absl::Status Database::DeleteTodoGroup(const std::string& group_name) {
  std::string sql = "DELETE FROM todos WHERE group_name = ?";
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

  (void)stmt->BindText(1, group_name);

  return stmt->Run();
}

absl::Status Database::AddMemo(const std::string& content, const std::string& semantic_tags) {
  auto stmt_or = Prepare("INSERT INTO llm_memos (content, semantic_tags) VALUES (?, ?)");
  if (!stmt_or.ok()) return stmt_or.status();
  auto& stmt = *stmt_or;
  (void)stmt->BindText(1, content);
  (void)stmt->BindText(2, semantic_tags);
  return stmt->Run();
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

absl::StatusOr<std::string> Database::Query(const std::string& sql) {
  auto stmt_or = Prepare(sql);
  CHECK_OK(stmt_or.status());
  auto& stmt = *stmt_or;

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
  return results.dump();
}

}  // namespace slop
