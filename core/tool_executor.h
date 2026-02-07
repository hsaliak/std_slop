#ifndef SLOP_SQL_TOOL_EXECUTOR_H_
#define SLOP_SQL_TOOL_EXECUTOR_H_

#include <memory>
#include <optional>
#include <string>

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

  void SetSessionId(const std::string& session_id) { session_id_ = session_id; }

 private:
  explicit ToolExecutor(Database* db) : db_(db) {}

 public:
  absl::StatusOr<std::string> Execute(const std::string& name, const nlohmann::json& args,
                                      std::shared_ptr<CancellationRequest> cancellation = nullptr);

 private:
  Database* db_;
  std::string session_id_;

  absl::StatusOr<std::string> ListDirectory(const ListDirectoryRequest& req,
                                            std::shared_ptr<CancellationRequest> cancellation);
  absl::StatusOr<std::string> ManageScratchpad(const ManageScratchpadRequest& req);
  absl::StatusOr<std::string> DescribeDb();
  absl::StatusOr<std::string> UseSkill(const UseSkillRequest& req);

  absl::StatusOr<std::string> Grep(const GrepRequest& req, std::shared_ptr<CancellationRequest> cancellation);
  absl::StatusOr<std::string> ReadFile(const ReadFileRequest& req);
  absl::StatusOr<std::string> WriteFile(const WriteFileRequest& req);
  absl::StatusOr<std::string> ApplyPatch(const ApplyPatchRequest& req);
  absl::StatusOr<std::string> ExecuteBash(const ExecuteBashRequest& req,
                                          std::shared_ptr<CancellationRequest> cancellation);
  absl::StatusOr<std::string> QueryDb(const QueryDbRequest& req);
  absl::StatusOr<std::string> SearchCode(const SearchCodeRequest& req,
                                         std::shared_ptr<CancellationRequest> cancellation);
  absl::StatusOr<std::string> GitGrep(const GitGrepRequest& req, std::shared_ptr<CancellationRequest> cancellation);
  absl::StatusOr<std::string> SaveMemo(const SaveMemoRequest& req);
  absl::StatusOr<std::string> RetrieveMemos(const RetrieveMemosRequest& req);

  absl::StatusOr<std::string> GitBranchStaging(const GitBranchStagingRequest& req);
  absl::StatusOr<std::string> GitCommitPatch(const GitCommitPatchRequest& req);
  absl::StatusOr<std::string> GitFormatPatchSeries(const GitFormatPatchSeriesRequest& req);
  absl::StatusOr<std::string> GitFinalizeSeries(const GitFinalizeSeriesRequest& req);
  absl::StatusOr<std::string> GitVerifySeries(const GitVerifySeriesRequest& req,
                                              std::shared_ptr<CancellationRequest> cancellation);
  absl::StatusOr<std::string> GitRerollPatch(const GitRerollPatchRequest& req);
};

}  // namespace slop

#endif  // SLOP_SQL_TOOL_EXECUTOR_H_
