#ifndef SLOP_TOOL_DISPATCHER_H_
#define SLOP_TOOL_DISPATCHER_H_

#include <atomic>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "nlohmann/json.hpp"

#include "core/cancellation.h"

namespace slop {

/**
 * @brief A handle to a background tool execution.
 */
class ToolJob {
 public:
  ToolJob(const std::string& id, const std::string& name) : id_(id), name_(name) {}

  bool IsReady() {
    absl::MutexLock lock(&mu_);
    return ready_;
  }

  absl::StatusOr<std::string> Wait() {
    mu_.LockWhen(absl::Condition(&ready_));
    auto res = result_;
    mu_.Unlock();
    return res;
  }

  void SetResult(absl::StatusOr<std::string> res) {
    absl::MutexLock lock(&mu_);
    result_ = std::move(res);
    ready_ = true;
  }

  const std::string& id() const { return id_; }
  const std::string& name() const { return name_; }

 private:
  const std::string id_;
  const std::string name_;
  absl::Mutex mu_;
  bool ready_ ABSL_GUARDED_BY(mu_) = false;
  absl::StatusOr<std::string> result_ ABSL_GUARDED_BY(mu_);
};

/**
 * @brief Dispatches tool calls in parallel by spawning new threads for each call.
 */
class ToolDispatcher {
 public:
  struct Call {
    std::string id;
    std::string name;
    nlohmann::json args;
  };

  struct Result {
    std::string id;
    std::string name;
    absl::StatusOr<std::string> output;
  };

  using ToolFunc = std::function<absl::StatusOr<std::string>(const std::string& name, const nlohmann::json& args,
                                                             std::shared_ptr<CancellationRequest> cancellation)>;

  /**
   * @param executor_func The function to call to execute a tool. Must be thread-safe.
   */
  explicit ToolDispatcher(ToolFunc executor_func);
  ~ToolDispatcher();

  /**
   * @brief Submits a single tool call for background execution.
   * @param call The tool call to execute.
   * @param cancellation Optional cancellation request.
   * @return A ToolJob handle to monitor and wait for the result.
   */
  std::shared_ptr<ToolJob> Submit(const Call& call, std::shared_ptr<CancellationRequest> cancellation = nullptr);

  /**
   * @brief Executes a batch of tool calls in parallel.
   * Blocks until all calls are complete or cancelled.
   * @param calls The list of tool calls to execute.
   * @param cancellation The cancellation request to monitor.
   */
  std::vector<Result> Dispatch(const std::vector<Call>& calls, std::shared_ptr<CancellationRequest> cancellation);

 private:
  void PruneThreads() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  ToolFunc executor_func_;

  absl::Mutex mu_;
  struct JobThread {
    std::thread thread;
    std::shared_ptr<ToolJob> job;
  };
  std::vector<JobThread> threads_ ABSL_GUARDED_BY(mu_);
};

}  // namespace slop

#endif  // SLOP_TOOL_DISPATCHER_H_
