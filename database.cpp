#include "database.h"
#include <sqlite3.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include "absl/strings/substitute.h"
#include "absl/strings/str_split.h"
#include "absl/strings/numbers.h"

namespace slop {

absl::Status Database::Init(const std::string& db_path) {
  sqlite3* raw_db = nullptr;
  int rc = sqlite3_open(db_path.c_str(), &raw_db);
  db_.reset(raw_db);
  if (rc != SQLITE_OK) {
    return absl::InternalError("Failed to open database: " + std::string(sqlite3_errmsg(db_.get())));
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
        group_id TEXT
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
        state_blob TEXT,
        last_updated TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    );
  )";

  absl::Status s = Execute(schema);
  if (!s.ok()) return s;

  // Migrations
  (void)Execute("ALTER TABLE sessions ADD COLUMN context_size INTEGER DEFAULT 5");

  s = RegisterDefaultTools();
  if (!s.ok()) return s;

  return absl::OkStatus();
}


absl::Status Database::RegisterDefaultTools() {
    std::vector<Tool> default_tools = {
        {"grep_tool", "Search for a pattern in the codebase using grep. Delegates to git_grep_tool if available in a git repository. Returns matching lines with context.",
         R"({"type":"object","properties":{"pattern":{"type":"string"},"path":{"type":"string"},"context":{"type":"integer","description":"Number of lines of context to show around matches"}},"required":["pattern"]})", true},
        {"git_grep_tool", "Comprehensive search using git grep. Optimized for git repositories, honors .gitignore, and can search history.",
         R"({
           "type": "object",
           "properties": {
             "pattern": {"type": "string", "description": "The pattern to search for."},
             "patterns": {"type": "array", "items": {"type": "string"}, "description": "Multiple patterns to search for (using -e)."},
             "all_match": {"type": "boolean", "default": false, "description": "Limit matches to files that match all patterns."},
             "path": {"type": "string", "description": "Limit search to specific path or file pattern."},
             "case_insensitive": {"type": "boolean", "default": false},
             "line_number": {"type": "boolean", "default": true},
             "count": {"type": "boolean", "default": false, "description": "Count occurrences per file."},
             "context": {"type": "integer", "default": 0, "description": "Show n lines of context."},
             "after": {"type": "integer", "default": 0, "description": "Show n lines after match."},
             "before": {"type": "integer", "default": 0, "description": "Show n lines before match."},
             "show_function": {"type": "boolean", "default": false, "description": "Show function/method context."},
             "files_with_matches": {"type": "boolean", "default": false, "description": "Only show file names."},
             "word_regexp": {"type": "boolean", "default": false, "description": "Match whole words only."},
             "pcre": {"type": "boolean", "default": false, "description": "Use Perl-compatible regular expressions."},
             "branch": {"type": "string", "description": "Search in a specific branch, tag, or commit."},
             "cached": {"type": "boolean", "default": false, "description": "Search in the staging area (index) instead of working tree."}
           }
         })", true},
        {"read_file", "Read the content of a file from the local filesystem. Returns content with line numbers.",
         R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})", true},
        {"write_file", "Write content to a file in the local filesystem.",
         R"({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]})", true},
        {"execute_bash", "Execute a bash command on the local system.",
         R"({"type":"object","properties":{"command":{"type":"string"}},"required":["command"]})", true},
        {"search_code", "Search for code snippets in the codebase using grep.",
         R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"]})", true},
        {"query_db", "Query the local SQLite database using SQL.",
         R"({"type":"object","properties":{"sql":{"type":"string"}},"required":["sql"]})", true}
    };

    for (const auto& t : default_tools) {
        absl::Status s = RegisterTool(t);
        if (!s.ok()) return s;
    }
    return absl::OkStatus();
}

absl::Status Database::AppendMessage(const std::string& session_id,
                                     const std::string& role,
                                     const std::string& content,
                                     const std::string& tool_call_id,
                                     const std::string& status,
                                     const std::string& group_id) {
  const char* sql = "INSERT INTO messages (session_id, role, content, tool_call_id, status, group_id) VALUES (?, ?, ?, ?, ?, ?)";
  sqlite3_stmt* raw_stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr);
  UniqueStmt stmt(raw_stmt);
  if (rc != SQLITE_OK) return absl::InternalError("Prepare error: " + std::string(sqlite3_errmsg(db_.get())));

  sqlite3_bind_text(stmt.get(), 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 2, role.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 3, content.c_str(), -1, SQLITE_TRANSIENT);
  if (tool_call_id.empty()) sqlite3_bind_null(stmt.get(), 4);
  else sqlite3_bind_text(stmt.get(), 4, tool_call_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 5, status.c_str(), -1, SQLITE_TRANSIENT);
  if (group_id.empty()) sqlite3_bind_null(stmt.get(), 6);
  else sqlite3_bind_text(stmt.get(), 6, group_id.c_str(), -1, SQLITE_TRANSIENT);

  if (sqlite3_step(stmt.get()) != SQLITE_DONE) return absl::InternalError("Execute error: " + std::string(sqlite3_errmsg(db_.get())));
  return absl::OkStatus();
}


absl::Status Database::UpdateMessageStatus(int id, const std::string& status) {
  const char* sql = "UPDATE messages SET status = ? WHERE id = ?";
  sqlite3_stmt* raw_stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr);
  UniqueStmt stmt(raw_stmt);
  if (rc != SQLITE_OK) return absl::InternalError("Prepare error");

  sqlite3_bind_text(stmt.get(), 1, status.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt.get(), 2, id);

  if (sqlite3_step(stmt.get()) != SQLITE_DONE) return absl::InternalError("Execute error");
  return absl::OkStatus();
}

absl::StatusOr<std::vector<Database::Message>> Database::GetConversationHistory(const std::string& session_id, bool include_dropped) {
  std::string sql = "SELECT id, session_id, role, content, tool_call_id, status, created_at, group_id FROM messages WHERE session_id = ?";
  if (!include_dropped) {
    sql += " AND status != 'dropped'";
  }
  sql += " ORDER BY created_at ASC";

  sqlite3_stmt* raw_stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr);
  UniqueStmt stmt(raw_stmt);
  if (rc != SQLITE_OK) return absl::InternalError("Prepare error");

  sqlite3_bind_text(stmt.get(), 1, session_id.c_str(), -1, SQLITE_TRANSIENT);

  std::vector<Message> messages;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    Message m;
    m.id = sqlite3_column_int(stmt.get(), 0);
    m.session_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
    m.role = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
    m.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
    const char* tcid = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
    if (tcid) m.tool_call_id = tcid;
    m.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
    m.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 6));
    const char* gid = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 7));
    if (gid) m.group_id = gid;
    messages.push_back(m);
  }
  return messages;
}

absl::StatusOr<std::vector<Database::Message>> Database::GetMessagesByGroups(const std::vector<std::string>& group_ids) {
    if (group_ids.empty()) return std::vector<Message>();
    
    std::string placeholders;
    for (size_t i = 0; i < group_ids.size(); ++i) {
        placeholders += (i == 0 ? "?" : ", ?");
    }
    
    std::string sql = "SELECT id, session_id, role, content, tool_call_id, status, created_at, group_id FROM messages WHERE group_id IN (" + placeholders + ") ORDER BY created_at ASC";
    
    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
    UniqueStmt stmt(raw_stmt);
    
    for (int i = 0; i < group_ids.size(); ++i) {
        sqlite3_bind_text(stmt.get(), i + 1, group_ids[i].c_str(), -1, SQLITE_TRANSIENT);
    }
    
    std::vector<Message> messages;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        Message m;
        m.id = sqlite3_column_int(stmt.get(), 0);
        m.session_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        m.role = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        m.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
        const char* tcid = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
        if (tcid) m.tool_call_id = tcid;
        m.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
        m.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 6));
        const char* gid = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 7));
        if (gid) m.group_id = gid;
        messages.push_back(m);
    }
    return messages;
}

absl::Status Database::RecordUsage(const std::string& session_id, const std::string& model, int prompt_tokens, int completion_tokens) {
  const char* sql = "INSERT INTO usage (session_id, model, prompt_tokens, completion_tokens, total_tokens) VALUES (?, ?, ?, ?, ?)";
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
  UniqueStmt stmt(raw_stmt);

  sqlite3_bind_text(stmt.get(), 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 2, model.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt.get(), 3, prompt_tokens);
  sqlite3_bind_int(stmt.get(), 4, completion_tokens);
  sqlite3_bind_int(stmt.get(), 5, prompt_tokens + completion_tokens);

  if (sqlite3_step(stmt.get()) != SQLITE_DONE) return absl::InternalError("Execute error");
  return absl::OkStatus();
}

absl::StatusOr<Database::TotalUsage> Database::GetTotalUsage(const std::string& session_id) {
  std::string sql = "SELECT SUM(prompt_tokens), SUM(completion_tokens), SUM(total_tokens) FROM usage";
  if (!session_id.empty()) {
    sql += " WHERE session_id = ?";
  }

  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
  UniqueStmt stmt(raw_stmt);

  if (!session_id.empty()) {
    sqlite3_bind_text(stmt.get(), 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
  }

  TotalUsage usage = {0, 0, 0};
  if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    usage.prompt_tokens = sqlite3_column_int(stmt.get(), 0);
    usage.completion_tokens = sqlite3_column_int(stmt.get(), 1);
    usage.total_tokens = sqlite3_column_int(stmt.get(), 2);
  }
  return usage;
}

absl::Status Database::RegisterTool(const Tool& tool) {
  const char* sql = "INSERT OR REPLACE INTO tools (name, description, json_schema, is_enabled) VALUES (?, ?, ?, ?)";
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
  UniqueStmt stmt(raw_stmt);

  sqlite3_bind_text(stmt.get(), 1, tool.name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 2, tool.description.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 3, tool.json_schema.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt.get(), 4, tool.is_enabled ? 1 : 0);

  if (sqlite3_step(stmt.get()) != SQLITE_DONE) return absl::InternalError("Execute error");
  return absl::OkStatus();
}

absl::StatusOr<std::vector<Database::Tool>> Database::GetEnabledTools() {
  const char* sql = "SELECT name, description, json_schema, is_enabled FROM tools WHERE is_enabled = 1";
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
  UniqueStmt stmt(raw_stmt);

  std::vector<Tool> tools;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    Tool t;
    t.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    t.description = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
    t.json_schema = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
    t.is_enabled = sqlite3_column_int(stmt.get(), 3) != 0;
    tools.push_back(t);
  }
  return tools;
}

absl::Status Database::RegisterSkill(const Skill& skill) {
  const char* sql = "INSERT OR REPLACE INTO skills (name, description, system_prompt_patch) VALUES (?, ?, ?)";
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
  UniqueStmt stmt(raw_stmt);

  sqlite3_bind_text(stmt.get(), 1, skill.name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 2, skill.description.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 3, skill.system_prompt_patch.c_str(), -1, SQLITE_TRANSIENT);

  if (sqlite3_step(stmt.get()) != SQLITE_DONE) return absl::InternalError("Execute error");
  return absl::OkStatus();
}

absl::Status Database::DeleteSkill(const std::string& name_or_id) {
    std::string sql = "DELETE FROM skills WHERE name = ? OR id = ?";
    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
    UniqueStmt stmt(raw_stmt);
    
    sqlite3_bind_text(stmt.get(), 1, name_or_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, name_or_id.c_str(), -1, SQLITE_TRANSIENT);
    
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return absl::InternalError("Execute error");
    return absl::OkStatus();
}

absl::StatusOr<std::vector<Database::Skill>> Database::GetSkills() {
  const char* sql = "SELECT id, name, description, system_prompt_patch FROM skills";
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
  UniqueStmt stmt(raw_stmt);

  std::vector<Skill> skills;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    Skill s;
    s.id = sqlite3_column_int(stmt.get(), 0);
    s.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
    s.description = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
    s.system_prompt_patch = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
    skills.push_back(s);
  }
  return skills;
}

absl::Status Database::SetContextWindow(const std::string& session_id, int size) {
    const char* sql = "INSERT INTO sessions (id, context_size) VALUES (?, ?) ON CONFLICT(id) DO UPDATE SET context_size = excluded.context_size";
    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
    UniqueStmt stmt(raw_stmt);
    
    sqlite3_bind_text(stmt.get(), 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 2, size);
    
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return absl::InternalError("Execute error");
    return absl::OkStatus();
}

absl::StatusOr<Database::ContextSettings> Database::GetContextSettings(const std::string& session_id) {
    const char* sql = "SELECT context_size FROM sessions WHERE id = ?";
    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
    UniqueStmt stmt(raw_stmt);
    
    sqlite3_bind_text(stmt.get(), 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    
    ContextSettings s = {5}; // Default
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        s.size = sqlite3_column_int(stmt.get(), 0);
    }
    return s;
}

absl::Status Database::SetSessionState(const std::string& session_id, const std::string& state_blob) {
    const char* sql = "INSERT INTO session_state (session_id, state_blob, last_updated) VALUES (?, ?, CURRENT_TIMESTAMP) "
                      "ON CONFLICT(session_id) DO UPDATE SET state_blob = excluded.state_blob, last_updated = CURRENT_TIMESTAMP";
    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
    UniqueStmt stmt(raw_stmt);
    
    sqlite3_bind_text(stmt.get(), 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, state_blob.c_str(), -1, SQLITE_TRANSIENT);
    
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return absl::InternalError("Execute error");
    return absl::OkStatus();
}

absl::StatusOr<std::string> Database::GetSessionState(const std::string& session_id) {
    const char* sql = "SELECT state_blob FROM session_state WHERE session_id = ?";
    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
    UniqueStmt stmt(raw_stmt);
    
    sqlite3_bind_text(stmt.get(), 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const char* blob = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        return blob ? std::string(blob) : "";
    }
    return absl::NotFoundError("No state found for session: " + session_id);
}

absl::StatusOr<std::string> Database::Query(const std::string& sql) {
  sqlite3_stmt* raw_stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr);
  UniqueStmt stmt(raw_stmt);
  if (rc != SQLITE_OK) return absl::InternalError("Query error: " + std::string(sqlite3_errmsg(db_.get())));

  nlohmann::json results = nlohmann::json::array();
  int cols = sqlite3_column_count(stmt.get());
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    nlohmann::json row;
    for (int i = 0; i < cols; ++i) {
      const char* name = sqlite3_column_name(stmt.get(), i);
      int type = sqlite3_column_type(stmt.get(), i);
      if (type == SQLITE_INTEGER) row[name] = sqlite3_column_int64(stmt.get(), i);
      else if (type == SQLITE_FLOAT) row[name] = sqlite3_column_double(stmt.get(), i);
      else if (type == SQLITE_NULL) row[name] = nullptr;
      else row[name] = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), i));
    }
    results.push_back(row);
  }
  return results.dump(2);
}

absl::StatusOr<std::string> Database::GetLastGroupId(const std::string& session_id) {
  const char* sql = "SELECT group_id FROM messages WHERE session_id = ? AND group_id IS NOT NULL ORDER BY id DESC LIMIT 1";
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
  UniqueStmt stmt(raw_stmt);
  sqlite3_bind_text(stmt.get(), 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      const char* gid = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
      return gid ? std::string(gid) : "";
  }
  return absl::NotFoundError("No messages found for this session");
}

absl::Status Database::DeleteSession(const std::string& session_id) {
  const char* begin_sql = "BEGIN TRANSACTION";
  const char* rollback_sql = "ROLLBACK";
  const char* commit_sql = "COMMIT";

  if (sqlite3_exec(db_.get(), begin_sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
    return absl::InternalError("Failed to begin transaction");
  }

  auto delete_from = [&](const std::string& table, const std::string& col) -> absl::Status {
    std::string sql = "DELETE FROM " + table + " WHERE " + col + " = ?";
    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr) != SQLITE_OK) {
      return absl::InternalError("Prepare error deleting from " + table);
    }
    UniqueStmt stmt(raw_stmt);
    sqlite3_bind_text(stmt.get(), 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
      return absl::InternalError("Execute error deleting from " + table);
    }
    return absl::OkStatus();
  };

  absl::Status s;
  s = delete_from("messages", "session_id");
  if (!s.ok()) { (void)sqlite3_exec(db_.get(), rollback_sql, nullptr, nullptr, nullptr); return s; }
  s = delete_from("usage", "session_id");
  if (!s.ok()) { (void)sqlite3_exec(db_.get(), rollback_sql, nullptr, nullptr, nullptr); return s; }
  s = delete_from("session_state", "session_id");
  if (!s.ok()) { (void)sqlite3_exec(db_.get(), rollback_sql, nullptr, nullptr, nullptr); return s; }
  s = delete_from("sessions", "id");
  if (!s.ok()) { (void)sqlite3_exec(db_.get(), rollback_sql, nullptr, nullptr, nullptr); return s; }

  if (sqlite3_exec(db_.get(), commit_sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
    return absl::InternalError("Failed to commit transaction");
  }

  return absl::OkStatus();
}

absl::Status Database::Execute(const std::string& sql) {
  char* errmsg = nullptr;
  int rc = sqlite3_exec(db_.get(), sql.c_str(), nullptr, nullptr, &errmsg);
  if (rc != SQLITE_OK) {
    std::string error = errmsg ? errmsg : "Unknown error";
    sqlite3_free(errmsg);
    return absl::InternalError("SQL error: " + error);
  }
  return absl::OkStatus();
}

}  // namespace slop
