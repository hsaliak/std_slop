#ifndef SLOP_MCP_GATEWAY_BROKER_H_
#define SLOP_MCP_GATEWAY_BROKER_H_

#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "js_runtime/runtime.h"
#include "mcp/gateway/catalog.h"
#include "mcp/gateway/scheduler.h"

namespace slop::mcp::gateway {

class ParallelBroker final : public js_runtime::AsyncToolBroker {
 public:
  explicit ParallelBroker(std::unique_ptr<Scheduler> scheduler) : scheduler_(std::move(scheduler)) {}
  static absl::StatusOr<std::shared_ptr<ParallelBroker>> Create(Catalog catalog);

  absl::Status Submit(const js_runtime::ToolRequest& request) override;
  std::vector<js_runtime::ToolCompletion> TakeCompletions() override;
  void WaitForCompletion(std::chrono::milliseconds duration) override;
  void CancelPending() override;
  nlohmann::json PublicCatalog() const override { return scheduler_->PublicCatalog(); }
  absl::StatusOr<nlohmann::json> Help(const std::string& alias, const std::string& tool) const override {
    return scheduler_->Help(alias, tool);
  }

 private:
  std::unique_ptr<Scheduler> scheduler_;
};

class SerialBroker final : public js_runtime::AsyncToolBroker {
 public:
  explicit SerialBroker(Catalog catalog) : catalog_(std::move(catalog)) {}

  absl::Status Submit(const js_runtime::ToolRequest& request) override;
  std::vector<js_runtime::ToolCompletion> TakeCompletions() override;
  void WaitForCompletion(std::chrono::milliseconds duration) override;
  void CancelPending() override;
  nlohmann::json PublicCatalog() const override { return catalog_.PublicCatalog(); }
  absl::StatusOr<nlohmann::json> Help(const std::string& alias, const std::string& tool) const override {
    return catalog_.Help(alias, tool);
  }

 private:
  void ProcessOne();

  Catalog catalog_;
  std::deque<js_runtime::ToolRequest> queued_;
  std::deque<js_runtime::ToolCompletion> completed_;
};

}  // namespace slop::mcp::gateway

#endif  // SLOP_MCP_GATEWAY_BROKER_H_
