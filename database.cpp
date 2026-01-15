#include "database.h"
#include <iostream>
#include <nlohmann/json.hpp>

namespace sentinel {

absl::Status Database::Init(const std::string& db_path) {
  sqlite3* raw_db = nullptr;
  int rc = sqlite3_open(db_path.c_str(), &raw_db);
  db_.reset(raw_db);
  if (rc != SQLITE_OK) {
    return absl::InternalError("Failed to open database: " + std::string(sqlite3_errmsg(db_.get())));
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
        system_prompt_patch TEXT,
        required_tools TEXT
    );

    CREATE TABLE IF NOT EXISTS sessions (
        id TEXT PRIMARY KEY,
        context_mode TEXT DEFAULT 'FULL',
        context_size INTEGER DEFAULT 5
    );

    CREATE VIRTUAL TABLE IF NOT EXISTS code_search USING fts5(path, content);
    CREATE VIRTUAL TABLE IF NOT EXISTS group_search USING fts5(group_id UNINDEXED, content);
  )";

  // Migration: Add context columns to sessions if they don't exist
  (void)Execute("ALTER TABLE sessions ADD COLUMN context_mode TEXT DEFAULT 'FULL'");
  (void)Execute("ALTER TABLE sessions ADD COLUMN context_size INTEGER DEFAULT 5");

  return Execute(schema);
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
    Message msg;
    msg.id = sqlite3_column_int(stmt.get(), 0);
    msg.session_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
    msg.role = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
    msg.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
    const char* tcid = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
    if (tcid) msg.tool_call_id = tcid;
    msg.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
    msg.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 6));
    const char* gid = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 7));
    if (gid) msg.group_id = gid;
    messages.push_back(std::move(msg));
  }
  return messages;
}

absl::StatusOr<std::vector<Database::Message>> Database::GetMessagesByGroups(const std::vector<std::string>& group_ids) {
    if (group_ids.empty()) return std::vector<Message>();
    
    std::string sql = "SELECT id, session_id, role, content, tool_call_id, status, created_at, group_id FROM messages WHERE group_id IN (";
    for (size_t i = 0; i < group_ids.size(); ++i) {
        sql += "?";
        if (i < group_ids.size() - 1) sql += ",";
    }
    sql += ") ORDER BY created_at ASC";

    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
    UniqueStmt stmt(raw_stmt);

    for (int i = 0; i < (int)group_ids.size(); ++i) {
        sqlite3_bind_text(stmt.get(), i + 1, group_ids[i].c_str(), -1, SQLITE_TRANSIENT);
    }

    std::vector<Message> messages;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        Message msg;
        msg.id = sqlite3_column_int(stmt.get(), 0);
        msg.session_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        msg.role = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        msg.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
        const char* tcid = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
        if (tcid) msg.tool_call_id = tcid;
        msg.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
        msg.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 6));
        const char* gid = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 7));
        if (gid) msg.group_id = gid;
        messages.push_back(std::move(msg));
    }
    return messages;
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
    Tool tool;
    tool.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    tool.description = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
    tool.json_schema = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
    tool.is_enabled = sqlite3_column_int(stmt.get(), 3) != 0;
    tools.push_back(std::move(tool));
  }
  return tools;
}

absl::Status Database::RegisterSkill(const Skill& skill) {
  const char* sql = "INSERT INTO skills (name, description, system_prompt_patch, required_tools) VALUES (?, ?, ?, ?) ON CONFLICT(name) DO UPDATE SET description=excluded.description, system_prompt_patch=excluded.system_prompt_patch, required_tools=excluded.required_tools";
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
  UniqueStmt stmt(raw_stmt);
  sqlite3_bind_text(stmt.get(), 1, skill.name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 2, skill.description.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 3, skill.system_prompt_patch.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 4, skill.required_tools.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) return absl::InternalError("Execute error");
  return absl::OkStatus();
}

absl::StatusOr<std::vector<Database::Skill>> Database::GetSkills() {
  const char* sql = "SELECT id, name, description, system_prompt_patch, required_tools FROM skills";
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
  UniqueStmt stmt(raw_stmt);
  std::vector<Skill> skills;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    Skill skill;
    skill.id = sqlite3_column_int(stmt.get(), 0);
    skill.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
    skill.description = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
    skill.system_prompt_patch = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
    skill.required_tools = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
    skills.push_back(std::move(skill));
  }
  return skills;
}

absl::Status Database::SetContextMode(const std::string& session_id, ContextMode mode, int size) {
  const char* sql = "INSERT INTO sessions (id, context_mode, context_size) VALUES (?, ?, ?) ON CONFLICT(id) DO UPDATE SET context_mode = excluded.context_mode, context_size = excluded.context_size";
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
  UniqueStmt stmt(raw_stmt);
  sqlite3_bind_text(stmt.get(), 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
  std::string mode_str = (mode == ContextMode::FULL) ? "FULL" : "FTS_RANKED";
  sqlite3_bind_text(stmt.get(), 2, mode_str.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt.get(), 3, size);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) return absl::InternalError("Execute error");
  return absl::OkStatus();
}

absl::StatusOr<Database::ContextSettings> Database::GetContextSettings(const std::string& session_id) {
  const char* sql = "SELECT context_mode, context_size FROM sessions WHERE id = ?";
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
  UniqueStmt stmt(raw_stmt);
  sqlite3_bind_text(stmt.get(), 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    std::string mode_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    int size = sqlite3_column_int(stmt.get(), 1);
    ContextMode mode = (mode_str == "FTS_RANKED") ? ContextMode::FTS_RANKED : ContextMode::FULL;
    return ContextSettings{mode, size};
  }
  return ContextSettings{ContextMode::FULL, 5};
}

absl::Status Database::IndexGroup(const std::string& group_id, const std::string& content) {
  const char* sql = "INSERT OR REPLACE INTO group_search (group_id, content) VALUES (?, ?)";
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
  UniqueStmt stmt(raw_stmt);
  sqlite3_bind_text(stmt.get(), 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 2, content.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) return absl::InternalError("Execute error");
  return absl::OkStatus();
}

absl::StatusOr<std::vector<std::string>> Database::SearchGroups(const std::string& query, int limit) {
  const char* sql = "SELECT group_id FROM group_search WHERE content MATCH ? ORDER BY rank LIMIT ?";
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
  UniqueStmt stmt(raw_stmt);
  sqlite3_bind_text(stmt.get(), 1, query.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt.get(), 2, limit);
  std::vector<std::string> results;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    results.push_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0)));
  }
  return results;
}

absl::Status Database::IndexFile(const std::string& path, const std::string& content) {
  const char* sql = "INSERT OR REPLACE INTO code_search (path, content) VALUES (?, ?)";
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
  UniqueStmt stmt(raw_stmt);
  sqlite3_bind_text(stmt.get(), 1, path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 2, content.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) return absl::InternalError("Execute error");
  return absl::OkStatus();
}

absl::StatusOr<std::vector<std::pair<std::string, std::string>>> Database::SearchCode(const std::string& query) {
  const char* sql = "SELECT path, content FROM code_search WHERE code_search MATCH ? ORDER BY rank";
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db_.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error");
  UniqueStmt stmt(raw_stmt);
  sqlite3_bind_text(stmt.get(), 1, query.c_str(), -1, SQLITE_TRANSIENT);
  std::vector<std::pair<std::string, std::string>> results;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    results.push_back({reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0)), reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1))});
  }
  return results;
}

absl::StatusOr<std::string> Database::Query(const std::string& sql) {
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw_stmt, nullptr) != SQLITE_OK) return absl::InternalError("Prepare error: " + sql);
  UniqueStmt stmt(raw_stmt);
  nlohmann::json results = nlohmann::json::array();
  int cols = sqlite3_column_count(stmt.get());
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    nlohmann::json row;
    for (int i = 0; i < cols; i++) {
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

}  // namespace sentinel