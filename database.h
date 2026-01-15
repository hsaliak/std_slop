#ifndef SENTINEL_SQL_DATABASE_H_
#define SENTINEL_SQL_DATABASE_H_

#include <sqlite3.h>
#include <string>
#include <vector>
#include <memory>
#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace sentinel {

class Database {
 public:
  Database() : db_(nullptr) {}
  ~Database() = default;

  // Non-copyable
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  absl::Status Init(const std::string& db_path = ":memory:");
  absl::Status Execute(const std::string& sql);

  struct Message {
    int id;
    std::string session_id;
    std::string role;
    std::string content;
    std::string tool_call_id;
    std::string status;
    std::string created_at;
    std::string group_id;
  };

  absl::Status AppendMessage(const std::string& session_id,
                             const std::string& role,
                             const std::string& content,
                             const std::string& tool_call_id = "",
                             const std::string& status = "completed",
                             const std::string& group_id = "");

  absl::StatusOr<std::vector<Message>> GetConversationHistory(const std::string& session_id);

  struct Tool {
    std::string name;
    std::string description;
    std::string json_schema;
    bool is_enabled;
  };

  absl::Status RegisterTool(const Tool& tool);
  absl::StatusOr<std::vector<Tool>> GetEnabledTools();

  struct Skill {
    int id;
    std::string name;
    std::string description;
    std::string system_prompt_patch;
    std::string required_tools; // JSON array of tool names
  };

  absl::Status RegisterSkill(const Skill& skill);
  absl::StatusOr<std::vector<Skill>> GetSkills();

  // Context Mode Settings
  enum class ContextMode { FULL, FTS_RANKED };
  absl::Status SetContextMode(const std::string& session_id, ContextMode mode, int size);
  struct ContextSettings { ContextMode mode; int size; };
  absl::StatusOr<ContextSettings> GetContextSettings(const std::string& session_id);

  // Group-level Search (FTS5)
  absl::Status IndexGroup(const std::string& group_id, const std::string& content);
  absl::StatusOr<std::vector<std::string>> SearchGroups(const std::string& query, int limit);

  // FTS5 Code Search
  absl::Status IndexFile(const std::string& path, const std::string& content);
  absl::StatusOr<std::vector<std::pair<std::string, std::string>>> SearchCode(const std::string& query);

  absl::StatusOr<std::string> Query(const std::string& sql);

  sqlite3* GetRawDb() const { return db_.get(); }

  struct StmtDeleter {
    void operator()(sqlite3_stmt* stmt) const { if (stmt) sqlite3_finalize(stmt); }
  };
  using UniqueStmt = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

 private:
  struct SqliteDeleter {
    void operator()(sqlite3* db) const { if (db) sqlite3_close(db); }
  };
  std::unique_ptr<sqlite3, SqliteDeleter> db_;
};

}  // namespace sentinel

#endif  // SENTINEL_SQL_DATABASE_H_
