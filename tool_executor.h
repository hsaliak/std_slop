#ifndef SLOP_SQL_TOOL_EXECUTOR_H_
#define SLOP_SQL_TOOL_EXECUTOR_H_

#include "database.h"

#include <optional>
#include <string>

#include "absl/log/check.h"
#include "absl/status/statusor.h"

#include <nlohmann/json.hpp>

namespace slop {

class ToolExecutor {
 public:
  explicit ToolExecutor(Database* db) : db_(db) { CHECK_NE(db_, nullptr); }

  void SetSessionId(const std::string& session_id) { session_id_ = session_id; }

  absl::StatusOr<std::string> Execute(const std::string& name, const nlohmann::json& args);

 private:
  Database* db_;
  std::string session_id_;

  absl::StatusOr<std::string> ListDirectory(const nlohmann::json& args);
  absl::StatusOr<std::string> ManageScratchpad(const nlohmann::json& args);
  absl::StatusOr<std::string> DescribeDb();

  absl::StatusOr<std::string> Grep(const std::string& pattern, const std::string& path, int context);
  absl::StatusOr<std::string> ReadFile(const std::string& path, std::optional<int> start_line = std::nullopt,
                                       std::optional<int> end_line = std::nullopt, bool add_line_numbers = false);
  absl::StatusOr<std::string> WriteFile(const std::string& path, const std::string& content);
  absl::StatusOr<std::string> ApplyPatch(const std::string& path, const nlohmann::json& patches);
  absl::StatusOr<std::string> ExecuteBash(const std::string& command);
  absl::StatusOr<std::string> SearchCode(const std::string& query);
  absl::StatusOr<std::string> GitGrep(const nlohmann::json& args);
  absl::StatusOr<std::string> SaveMemo(const std::string& content, const std::vector<std::string>& tags);
  absl::StatusOr<std::string> RetrieveMemos(const std::vector<std::string>& tags);
};

}  // namespace slop

#endif  // SLOP_SQL_TOOL_EXECUTOR_H_
