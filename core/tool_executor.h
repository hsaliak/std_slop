#ifndef SLOP_SQL_TOOL_EXECUTOR_H_
#define SLOP_SQL_TOOL_EXECUTOR_H_

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/status/statusor.h"

#include "core/cancellation.h"
#include "core/database.h"
#include "core/tool_types.h"

#include <nlohmann/json.hpp>

namespace slop {

class ToolExecutor {
 public:
  static absl::StatusOr<std::unique_ptr<ToolExecutor>> Create(Database* db) {
    if (db == nullptr) {
      return absl::InvalidArgumentError("Database cannot be null");
    }
    return std::unique_ptr<ToolExecutor>(new ToolExecutor(db));
  }

  void SetSessionId(const std::string& session_id);
  void SetMailMode(bool enabled) { mail_mode_ = enabled; }
  const std::string& session_id() const { return session_id_; }

  bool IsSkillActive(const std::string& name);
  std::vector<std::string> GetActiveSkills();

  absl::StatusOr<std::string> Execute(const std::string& name, const nlohmann::json& args,
                                      std::shared_ptr<CancellationRequest> cancellation = nullptr);

  // Resolves the base branch for git operations.
  // Checks git config slop.basebranch, then defaults to main/master.
  absl::StatusOr<std::string> GetBaseBranch(const std::string& requested_base);

 private:
  explicit ToolExecutor(Database* db);

  Database* db_;
  std::string session_id_;
  bool mail_mode_ = false;





  absl::StatusOr<std::string> ApplyPatch(const ApplyPatchRequest& req);

  absl::StatusOr<std::string> RunLua(const RunLuaRequest& req,
                                     std::shared_ptr<CancellationRequest> cancellation,
                                     bool raw = false);

  using ToolHandler = std::function<absl::StatusOr<std::string>(
      const nlohmann::json&, std::shared_ptr<CancellationRequest>)>;
  absl::flat_hash_map<std::string, ToolHandler> dispatch_map_;
  absl::flat_hash_set<std::string> lua_tools_;


};

}  // namespace slop

#endif  // SLOP_SQL_TOOL_EXECUTOR_H_
