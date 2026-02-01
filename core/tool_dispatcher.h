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
#include "core/cancellation.h"
#include "nlohmann/json.hpp"

namespace slop {

/**
 * @brief Dispatches tool calls in parallel using a fixed thread pool.
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

  using ToolFunc = std::function<absl::StatusOr<std::string>(
      const std::string& name, const nlohmann::json& args, 
      std::shared_ptr<CancellationRequest> cancellation)>;

  /**
   * @param executor_func The function to call to execute a tool. Must be thread-safe.
   * @param num_threads Number of worker threads.
   */
  explicit ToolDispatcher(ToolFunc executor_func, int num_threads = 4);
  ~ToolDispatcher();

  /**
   * @brief Executes a batch of tool calls in parallel.
   * Blocks until all calls are complete or cancelled.
   * @param calls The list of tool calls to execute.
   * @param cancellation The cancellation request to monitor.
   */
  std::vector<Result> Dispatch(const std::vector<Call>& calls, 
                               std::shared_ptr<CancellationRequest> cancellation);

 private:
  void WorkerLoop();

  ToolFunc executor_func_;
  int num_threads_;
  std::vector<std::thread> workers_;

  absl::Mutex mu_;
  std::queue<std::function<void()>> tasks_ ABSL_GUARDED_BY(mu_);
  bool stop_ ABSL_GUARDED_BY(mu_) = false;
};

}  // namespace slop

#endif  // SLOP_TOOL_DISPATCHER_H_
