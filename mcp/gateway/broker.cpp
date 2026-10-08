#include "mcp/gateway/broker.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"

#include "mcp/gateway/result.h"

namespace slop::mcp::gateway {
namespace {

constexpr std::size_t kMaxQueuedCalls = 64;

}  // namespace

absl::StatusOr<std::shared_ptr<ParallelBroker>> ParallelBroker::Create(Catalog catalog,
                                                                         std::shared_ptr<TraceLogger> trace_logger) {
  auto scheduler = Scheduler::Create(std::move(catalog), std::move(trace_logger));
  if (!scheduler.ok()) return scheduler.status();
  return std::make_shared<ParallelBroker>(std::move(*scheduler));
}

absl::Status ParallelBroker::Submit(const js_runtime::ToolRequest& request) { return scheduler_->Submit(request); }

std::vector<js_runtime::ToolCompletion> ParallelBroker::TakeCompletions() { return scheduler_->TakeCompletions(); }

void ParallelBroker::WaitForCompletion(std::chrono::milliseconds duration) { scheduler_->WaitForCompletion(duration); }

void ParallelBroker::CancelPending() { scheduler_->CancelQueued(); }

absl::Status SerialBroker::Submit(const js_runtime::ToolRequest& request) {
  if (queued_.size() + completed_.size() >= kMaxQueuedCalls) {
    return absl::ResourceExhaustedError("native call queue is full");
  }
  queued_.push_back(request);
  return absl::OkStatus();
}

std::vector<js_runtime::ToolCompletion> SerialBroker::TakeCompletions() {
  std::vector<js_runtime::ToolCompletion> results;
  results.reserve(completed_.size());
  while (!completed_.empty()) {
    results.push_back(std::move(completed_.front()));
    completed_.pop_front();
  }
  return results;
}

void SerialBroker::WaitForCompletion(std::chrono::milliseconds) {
  while (!queued_.empty()) ProcessOne();
}

void SerialBroker::CancelPending() { queued_.clear(); }

void SerialBroker::ProcessOne() {
  js_runtime::ToolRequest request = std::move(queued_.front());
  queued_.pop_front();
  auto result = catalog_.Call(request.server, request.tool, request.arguments, request.deadline);
  if (!result.ok()) {
    completed_.push_back(
        {request.run_id, request.id, false, nullptr, BoundCompletionError(std::string(result.status().message())), "downstream"});
    return;
  }
  auto normalized = NormalizeToolResult(*result);
  if (!normalized.ok()) {
    completed_.push_back({request.run_id, request.id, false, nullptr,
                          BoundCompletionError(std::string(normalized.status().message())), "normalization"});
    return;
  }
  completed_.push_back({request.run_id, request.id, normalized->ok, std::move(normalized->value),
                        BoundCompletionError(std::move(normalized->error)), std::move(normalized->error_category)});
}

}  // namespace slop::mcp::gateway
