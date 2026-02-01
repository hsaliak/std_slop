#ifndef SLOP_SQL_DATABASE_H_
#define SLOP_SQL_DATABASE_H_

#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include <sqlite3.h>

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"

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
  absl::Status Execute(const std::string& sql, const std::vector<std::string>& params);

  template <typename... Args>
  absl::Status Execute(const std::string& sql, Args&&... args) {
    auto stmt_or = Prepare(sql);
    if (!stmt_or.ok()) return stmt_or.status();
    auto bind_status = (*stmt_or)->BindAll(std::forward<Args>(args)...);
    if (!bind_status.ok()) return bind_status;
    return (*stmt_or)->Run();
  }

  struct StmtDeleter {
    void operator()(sqlite3_stmt* stmt) const {
      if (stmt) sqlite3_finalize(stmt);
    }
  };
  using UniqueStmt = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

  class Statement {
   public:
    Statement(sqlite3* db, const std::string& sql) : db_(db), sql_(sql) {}

    absl::Status Prepare();
    absl::Status BindInt(int index, int value);
    absl::Status BindInt64(int index, int64_t value);
    absl::Status BindDouble(int index, double value);
    absl::Status BindText(int index, const std::string& value);
    absl::Status BindNull(int index);

    // Overloads for easier binding
    absl::Status Bind(int index, int value) { return BindInt(index, value); }
    absl::Status Bind(int index, int64_t value) { return BindInt64(index, value); }
    absl::Status Bind(int index, double value) { return BindDouble(index, value); }
    absl::Status Bind(int index, const std::string& value) { return BindText(index, value); }
    absl::Status Bind(int index, const char* value) { return BindText(index, value ? value : ""); }
    absl::Status Bind(int index, std::nullptr_t) { return BindNull(index); }

    template <typename... Args>
    absl::Status BindAll(Args&&... args) {
      return BindRecursive(1, std::forward<Args>(args)...);
    }

    absl::StatusOr<bool> Step();  // Returns true if a row is available (SQLITE_ROW)
    absl::Status Run();           // For operations that don't return rows (SQLITE_DONE)

    int ColumnInt(int index);
    int64_t ColumnInt64(int index);
    double ColumnDouble(int index);
    std::string ColumnText(int index);
    int ColumnType(int index);
    const char* ColumnName(int index);
    int ColumnCount();

   private:
    absl::Status BindRecursive(int /*index*/) { return absl::OkStatus(); }

    template <typename T, typename... Rest>
    absl::Status BindRecursive(int index, T&& first, Rest&&... rest) {
      auto status = Bind(index, std::forward<T>(first));
      if (!status.ok()) return status;
      return BindRecursive(index + 1, std::forward<Rest>(rest)...);
    }

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
    std::string parsing_strategy;
    int tokens;
  };

  /**
   * @brief Appends a new message to the conversation history.
   *
   * @param session_id The unique identifier for the session.
   * @param role The role of the message sender (e.g., "user", "assistant", "system").
   * @param content The content of the message.
   * @param tool_call_id Optional ID if the message is related to a tool call.
   * @param status The status of the message (e.g., "completed", "dropped").
   * @param group_id Optional ID to group related messages together.
   * @param parsing_strategy Optional strategy used to parse the message.
   * @param tokens The number of tokens used by this message.
   * @return absl::Status OK if successful, otherwise an error.
   */
  absl::Status AppendMessage(const std::string& session_id, const std::string& role, const std::string& content,
                             const std::string& tool_call_id = "", const std::string& status = "completed",
                             const std::string& group_id = "", const std::string& parsing_strategy = "",
                             int tokens = 0);
  absl::Status UpdateMessageStatus(int id, const std::string& status);

  absl::StatusOr<std::vector<Message>> GetConversationHistory(const std::string& session_id,
                                                              bool include_dropped = false, int window_size = 0);
  absl::StatusOr<std::vector<Message>> GetMessagesByGroups(const std::vector<std::string>& group_ids);
  absl::StatusOr<std::string> GetLastGroupId(const std::string& session_id);

  struct Usage {
    std::string session_id;
    std::string model;
    int prompt_tokens;
    int completion_tokens;
    int total_tokens;
    std::string created_at;
  };

  absl::Status RecordUsage(const std::string& session_id, const std::string& model, int prompt_tokens,
                           int completion_tokens);
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
    int call_count = 0;
  };

  absl::Status RegisterTool(const Tool& tool);
  absl::StatusOr<std::vector<Tool>> GetEnabledTools();

  struct Skill {
    int id;
    std::string name;
    std::string description;
    std::string system_prompt_patch;
    int activation_count = 0;
  };

  absl::Status RegisterSkill(const Skill& skill);
  absl::Status UpdateSkill(const Skill& skill);
  absl::Status DeleteSkill(const std::string& name_or_id);
  absl::StatusOr<std::vector<Skill>> GetSkills();
  absl::Status IncrementSkillActivationCount(const std::string& name_or_id);
  absl::Status IncrementToolCallCount(const std::string& name);

  absl::Status SetActiveSkills(const std::string& session_id, const std::vector<std::string>& skills);
  absl::StatusOr<std::vector<std::string>> GetActiveSkills(const std::string& session_id);

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

  absl::Status UpdateScratchpad(const std::string& session_id, const std::string& scratchpad);
  absl::StatusOr<std::string> GetScratchpad(const std::string& session_id);

  struct Memo {
    int id;
    std::string content;
    std::string semantic_tags;
    std::string created_at;
  };

  static std::vector<std::string> ExtractTags(const std::string& text);
  static bool IsStopWord(const std::string& word);

  absl::Status AddMemo(const std::string& content, const std::string& semantic_tags);
  absl::Status UpdateMemo(int id, const std::string& content, const std::string& semantic_tags);
  absl::Status DeleteMemo(int id);
  absl::StatusOr<Memo> GetMemo(int id);
  absl::StatusOr<std::vector<Memo>> GetMemosByTags(const std::vector<std::string>& tags);
  absl::StatusOr<std::vector<Memo>> GetAllMemos();
  // Full Text Search
  absl::StatusOr<std::string> Query(const std::string& sql);
  absl::StatusOr<std::string> Query(const std::string& sql, const std::vector<std::string>& params);

 private:
  absl::Status RegisterDefaultTools();
  absl::Status RegisterDefaultSkills();

  struct DbDeleter {
    void operator()(sqlite3* db) const {
      if (db) sqlite3_close(db);
    }
  };
  absl::Mutex mu_;
  std::unique_ptr<sqlite3, DbDeleter> db_ ABSL_GUARDED_BY(mu_);
};

}  // namespace slop

#endif  // SLOP_SQL_DATABASE_H_
