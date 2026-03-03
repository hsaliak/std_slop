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

class ToolDispatcher;

class ToolExecutor {
 public:
  static absl::StatusOr<std::unique_ptr<ToolExecutor>> Create(Database* db) {
    if (db == nullptr) {
      return absl::InvalidArgumentError("Database cannot be null");
    }
    return std::unique_ptr<ToolExecutor>(new ToolExecutor(db));
  }

  void SetSessionId(const std::string& session_id);
  void SetMailMode(bool enabled);
  const std::string& session_id() const { return session_id_; }

  bool IsSkillActive(const std::string& name);
  std::vector<std::string> GetActiveSkills();

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

 private:
  explicit ToolExecutor(Database* db);

  std::function<std::string(const std::string&)> ask_user_handler_;

  Database* db_;
  std::string session_id_;
  bool mail_mode_ = false;

  void RegisterTools();

  struct JsResult {
    std::string stdout_out;
    std::string return_value;
    bool has_js_return_value = false;

    std::string FullOutput() const {
      if (stdout_out.empty()) {
        return return_value;
      }
      if (return_value.empty()) {
        return stdout_out;
      }
      return stdout_out + "\n" + return_value;
    }
  };

  absl::StatusOr<std::string> HandleQueryDb(const nlohmann::json& args);
  absl::StatusOr<std::string> HandleRunLua(const nlohmann::json& args,
                                           std::shared_ptr<CancellationRequest> cancellation);
  absl::StatusOr<std::string> HandleRunJs(const nlohmann::json& args,
                                          std::shared_ptr<CancellationRequest> cancellation);

  absl::StatusOr<JsResult> RunJs(const RunJsRequest& req, std::shared_ptr<CancellationRequest> cancellation);

  absl::flat_hash_map<std::string, ToolHandler> dispatch_map_;
  std::unique_ptr<ToolDispatcher> dispatcher_;
};

}  // namespace slop

#endif  // SLOP_SQL_TOOL_EXECUTOR_H_
