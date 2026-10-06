#ifndef SLOP_MCP_GATEWAY_SCHEDULER_H_
#define SLOP_MCP_GATEWAY_SCHEDULER_H_

#include <chrono>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "js_runtime/runtime.h"
#include "mcp/gateway/catalog.h"

namespace slop::mcp::gateway {

class Scheduler {
 public:
  static absl::StatusOr<std::unique_ptr<Scheduler>> Create(Catalog catalog);
  explicit Scheduler(Catalog catalog) : catalog_(std::move(catalog)) {}
  ~Scheduler();

  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;

  absl::Status Submit(const js_runtime::ToolRequest& request);
  std::vector<js_runtime::ToolCompletion> TakeCompletions();
  void WaitForCompletion(std::chrono::milliseconds duration);
  void CancelQueued();
  nlohmann::json PublicCatalog() const { return catalog_.PublicCatalog(); }
  absl::StatusOr<nlohmann::json> Help(const std::string& alias, const std::string& tool) const {
    return catalog_.Help(alias, tool);
  }

 private:
  struct Lane {
    std::string alias;
    std::unique_ptr<Client> client;
    std::deque<js_runtime::ToolRequest> requests;
    std::thread worker;
  };

  absl::Status Start();
  void RunLane(Lane* lane);
  js_runtime::ToolCompletion Process(const js_runtime::ToolRequest& request);

  static constexpr std::size_t kMaxOutstandingCalls = 64;
  static constexpr std::size_t kMaxActiveCalls = 4;
  Catalog catalog_;
  mutable absl::Mutex mutex_;
  absl::CondVar work_available_;
  absl::CondVar completion_available_;
  std::vector<std::unique_ptr<Lane>> lanes_;
  std::map<std::string, Lane*> lane_by_alias_;
  std::deque<js_runtime::ToolCompletion> completions_;
  std::size_t queued_calls_ = 0;
  std::size_t active_calls_ = 0;
  std::size_t outstanding_calls_ = 0;
  bool stopping_ = false;
};

}  // namespace slop::mcp::gateway

#endif  // SLOP_MCP_GATEWAY_SCHEDULER_H_
