#ifndef SLOP_SQL_TOOL_EXECUTOR_H_
#define SLOP_SQL_TOOL_EXECUTOR_H_

#include <string>
#include <nlohmann/json.hpp>
#include "absl/status/statusor.h"
#include "database.h"

namespace slop {

class ToolExecutor {
 public:
  explicit ToolExecutor(Database* db) : db_(db) {}

  absl::StatusOr<std::string> Execute(const std::string& name, const nlohmann::json& args);

 private:
  Database* db_;

  absl::StatusOr<std::string> Grep(const std::string& pattern, const std::string& path, int context);
  absl::StatusOr<std::string> ReadFile(const std::string& path, bool add_line_numbers = false);
  absl::StatusOr<std::string> WriteFile(const std::string& path, const std::string& content);
  absl::StatusOr<std::string> ApplyPatch(const std::string& path, const nlohmann::json& patches);
  absl::StatusOr<std::string> ExecuteBash(const std::string& command);
  absl::StatusOr<std::string> SearchCode(const std::string& query);
  absl::StatusOr<std::string> GitGrep(const nlohmann::json& args);
};

} // namespace slop

#endif // SLOP_SQL_TOOL_EXECUTOR_H_
