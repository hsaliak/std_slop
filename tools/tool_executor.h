#ifndef SLOP_SQL_TOOL_EXECUTOR_H_
#define SLOP_SQL_TOOL_EXECUTOR_H_

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/memory/memory.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"

#include "core/cancellation.h"
#include "core/database.h"

#include <nlohmann/json.hpp>

namespace slop {

class ToolDispatcher;

class ToolExecutor {
 public:
  enum class ExecutionScope {
    kRoot,
    kSubquery,
  };

  static absl::StatusOr<std::unique_ptr<ToolExecutor>> Create(Database* db) {
    if (db == nullptr) {
      return absl::InvalidArgumentError("Database cannot be null");
    }
    return absl::WrapUnique(new ToolExecutor(db));
  }

  void SetSessionId(const std::string& session_id);
  void SetMailMode(bool enabled);
  const std::string& session_id() const { return session_id_; }
  void SetExecutionContext(ExecutionScope scope, int depth);

  bool IsSkillActive(const std::string& name);
  std::vector<std::string> GetActiveSkills();
  std::vector<std::string> GetRegisteredToolNamesForTest() const;

  void SetAskUserHandler(std::function<std::string(const std::string&)> handler) {
    ask_user_handler_ = std::move(handler);
  }

  absl::StatusOr<std::string> Execute(const std::string& name, const nlohmann::json& args,
                                      std::shared_ptr<CancellationRequest> cancellation = nullptr);

  // Resolves the base branch for git operations.
  // Resolves from database (staging_branches) or defaults to main.
  absl::StatusOr<std::string> GetBaseBranch(const std::string& requested_base);

  void SetDispatcher(std::unique_ptr<ToolDispatcher> dispatcher);
  ToolDispatcher* dispatcher() const { return dispatcher_.get(); }

  using ToolHandler =
      std::function<absl::StatusOr<std::string>(const nlohmann::json&, std::shared_ptr<CancellationRequest>)>;
  void RegisterTool(const std::string& name, ToolHandler handler);

  ~ToolExecutor();

  void InvalidateActiveSkillsCache();
  void RefreshActiveSkillsCacheIfNeeded();

 private:
  explicit ToolExecutor(Database* db);

  std::function<std::string(const std::string&)> ask_user_handler_;

  Database* db_;
  std::string session_id_;
  bool mail_mode_ = false;
  std::pair<ExecutionScope, int> execution_context_ = {ExecutionScope::kRoot, 0};

  void RegisterTools();

  absl::StatusOr<std::string> HandleQueryDb(const nlohmann::json& args);
  absl::StatusOr<std::string> HandleReadFile(const nlohmann::json& args);
  absl::StatusOr<std::string> HandleListDirectory(const nlohmann::json& args);
  absl::StatusOr<std::string> HandleDescribeDb(const nlohmann::json& args);
  absl::StatusOr<std::string> HandleGrep(const nlohmann::json& args);
  absl::StatusOr<std::string> HandleGitCreateStagingBranch(const nlohmann::json& args);
  absl::StatusOr<std::string> HandleGitCommitPatch(const nlohmann::json& args,
                                                   std::shared_ptr<CancellationRequest> cancellation);
  absl::StatusOr<std::string> HandleGitFormatPatchSeries(const nlohmann::json& args,
                                                         std::shared_ptr<CancellationRequest> cancellation);
  absl::StatusOr<std::string> HandleGitRerollPatch(const nlohmann::json& args,
                                                   std::shared_ptr<CancellationRequest> cancellation);
  absl::StatusOr<std::string> HandleGitVerifySeries(const nlohmann::json& args,
                                                    std::shared_ptr<CancellationRequest> cancellation);
  absl::StatusOr<std::string> HandleGitFinalizeSeries(const nlohmann::json& args,
                                                      std::shared_ptr<CancellationRequest> cancellation);
  absl::StatusOr<std::string> HandleExecuteBash(const nlohmann::json& args) const;
  absl::StatusOr<std::string> HandlePatchTool(const nlohmann::json& args) const;
  absl::StatusOr<std::string> HandleWriteFile(const nlohmann::json& args) const;
  absl::StatusOr<std::string> HandleReadScratchpad(const nlohmann::json& args);
  absl::StatusOr<std::string> HandleWriteScratchpad(const nlohmann::json& args);
  absl::StatusOr<std::string> HandleUseSkill(const nlohmann::json& args);
  absl::StatusOr<std::string> HandleRunJs(const nlohmann::json& args) const;

  absl::Status ValidateSubqueryPolicy(const std::string& tool_name) const;

  absl::flat_hash_map<std::string, ToolHandler> dispatch_map_;
  absl::Mutex active_skills_mu_;
  bool active_skills_cache_valid_ ABSL_GUARDED_BY(active_skills_mu_) = false;
  std::string active_skills_cache_session_id_ ABSL_GUARDED_BY(active_skills_mu_);
  std::vector<std::string> active_skills_cache_ ABSL_GUARDED_BY(active_skills_mu_);
  absl::flat_hash_set<std::string> active_skills_cache_set_ ABSL_GUARDED_BY(active_skills_mu_);
  std::unique_ptr<ToolDispatcher> dispatcher_;
};

}  // namespace slop

#endif  // SLOP_SQL_TOOL_EXECUTOR_H_
