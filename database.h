#ifndef SLOP_SQL_DATABASE_H_
#define SLOP_SQL_DATABASE_H_

#include <sqlite3.h>
#include <string>
#include <vector>
#include <memory>
#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace slop {

class Database {
 public:
  Database() : db_(nullptr) {}
  ~Database() = default;

  // Non-copyable
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  absl::Status Init(const std::string& db_path = ":memory:");
  absl::Status Execute(const std::string& sql);

  struct StmtDeleter {
    void operator()(sqlite3_stmt* stmt) const { if (stmt) sqlite3_finalize(stmt); }
  };
  using UniqueStmt = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

  class Statement {
   public:
    Statement(sqlite3* db, const std::string& sql) : db_(db), sql_(sql) {}
    
    absl::Status Prepare();
    absl::Status BindInt(int index, int value);
    absl::Status BindText(int index, const std::string& value);
    absl::Status BindNull(int index);
    
    absl::StatusOr<bool> Step(); // Returns true if a row is available (SQLITE_ROW)
    absl::Status Run();      // For operations that don't return rows (SQLITE_DONE)

    int ColumnInt(int index);
    int64_t ColumnInt64(int index);
    double ColumnDouble(int index);
    std::string ColumnText(int index);
    int ColumnType(int index);
    const char* ColumnName(int index);
    int ColumnCount();

   private:
    sqlite3* db_;
    std::string sql_;
    UniqueStmt stmt_;
  };

  absl::StatusOr<std::unique_ptr<Statement>> Prepare(const std::string& sql);

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
  absl::Status UpdateMessageStatus(int id, const std::string& status);

  absl::StatusOr<std::vector<Message>> GetConversationHistory(const std::string& session_id, bool include_dropped = false);
  absl::StatusOr<std::vector<Message>> GetMessagesByGroups(const std::vector<std::string>& group_ids);

  struct Usage {
    std::string session_id;
    std::string model;
    int prompt_tokens;
    int completion_tokens;
    int total_tokens;
    std::string created_at;
  };

  absl::Status RecordUsage(const std::string& session_id, const std::string& model, int prompt_tokens, int completion_tokens);
  struct TotalUsage {
    int prompt_tokens;
    int completion_tokens;
    int total_tokens;
  };
  absl::StatusOr<TotalUsage> GetTotalUsage(const std::string& session_id = "");

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
  };

  absl::Status RegisterSkill(const Skill& skill);
  absl::Status UpdateSkill(const Skill& skill);
  absl::Status DeleteSkill(const std::string& name_or_id);
  absl::StatusOr<std::vector<Skill>> GetSkills();

  // Context Settings
  absl::Status SetContextWindow(const std::string& session_id, int size);
  struct ContextSettings { 
      int size;
  };
  absl::StatusOr<ContextSettings> GetContextSettings(const std::string& session_id);

  // Session State Management
  absl::Status SetSessionState(const std::string& session_id, const std::string& state_blob);
  absl::StatusOr<std::string> GetSessionState(const std::string& session_id);

  // Session Deletion
  absl::Status DeleteSession(const std::string& session_id);

  struct Todo {
    int id;
    std::string group_name;
    std::string description;
    std::string status;
  };

  absl::Status AddTodo(const std::string& group_name, const std::string& description);
  absl::StatusOr<std::vector<Todo>> GetTodos(const std::string& group_name = "");
  absl::Status UpdateTodo(int id, const std::string& group_name, const std::string& description);
  absl::Status UpdateTodoStatus(int id, const std::string& group_name, const std::string& status);
  absl::Status DeleteTodoGroup(const std::string& group_name);

  absl::StatusOr<std::string> Query(const std::string& sql);

  absl::StatusOr<std::string> GetLastGroupId(const std::string& session_id);

  sqlite3* GetRawDb() const { return db_.get(); }

 private:
  absl::Status RegisterDefaultTools();
  absl::Status RegisterDefaultSkills();

  struct SqliteDeleter {
    void operator()(sqlite3* db) const { if (db) sqlite3_close(db); }
  };
  std::unique_ptr<sqlite3, SqliteDeleter> db_;
};

}  // namespace slop

#endif  // SLOP_SQL_DATABASE_H_
