#include "mcp/gateway/scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "mcp/gateway/result.h"

namespace slop::mcp::gateway {
namespace {

constexpr std::size_t kMaxErrorBytes = 4096;

std::string Bounded(std::string text) {
  if (text.size() > kMaxErrorBytes) text.resize(kMaxErrorBytes);
  return text;
}

}  // namespace

absl::StatusOr<std::unique_ptr<Scheduler>> Scheduler::Create(Catalog catalog,
                                                              std::shared_ptr<TraceLogger> trace_logger) {
  auto scheduler = std::make_unique<Scheduler>(std::move(catalog), std::move(trace_logger));
  absl::Status status = scheduler->Start();
  if (!status.ok()) return status;
  return scheduler;
}

Scheduler::~Scheduler() {
  {
    absl::MutexLock lock(mutex_);
    stopping_ = true;
    work_available_.SignalAll();
    completion_available_.SignalAll();
  }
  for (const std::unique_ptr<Lane>& lane : lanes_) {
    if (lane->worker.joinable()) lane->worker.join();
  }
}

absl::Status Scheduler::Start() {
  for (const std::string& alias : catalog_.server_aliases()) {
    auto lane = std::make_unique<Lane>();
    lane->alias = alias;
    lane->client = catalog_.TakeClient(alias);
    if (lane->client == nullptr) return absl::InternalError("failed to transfer a client to its scheduler lane");
    lane_by_alias_.emplace(alias, lane.get());
    lanes_.push_back(std::move(lane));
  }
  for (const std::unique_ptr<Lane>& lane : lanes_) {
    lane->worker = std::thread(&Scheduler::RunLane, this, lane.get());
  }
  return absl::OkStatus();
}

absl::Status Scheduler::Submit(const js_runtime::ToolRequest& request) {
  absl::MutexLock lock(mutex_);
  if (stopping_) return absl::FailedPreconditionError("scheduler is stopping");
  auto lane = lane_by_alias_.find(request.server);
  if (lane == lane_by_alias_.end()) return absl::PermissionDeniedError("server is not in the configured catalog");
  if (outstanding_calls_ >= kMaxOutstandingCalls) {
    return absl::ResourceExhaustedError("scheduler call limit exceeded");
  }
  lane->second->requests.push_back(request);
  ++queued_calls_;
  ++outstanding_calls_;
  work_available_.SignalAll();
  return absl::OkStatus();
}

std::vector<js_runtime::ToolCompletion> Scheduler::TakeCompletions() {
  absl::MutexLock lock(mutex_);
  std::vector<js_runtime::ToolCompletion> results;
  results.reserve(completions_.size());
  while (!completions_.empty()) {
    results.push_back(std::move(completions_.front()));
    completions_.pop_front();
  }
  outstanding_calls_ -= results.size();
  return results;
}

void Scheduler::WaitForCompletion(std::chrono::milliseconds duration) {
  if (duration <= std::chrono::milliseconds::zero()) return;
  absl::MutexLock lock(mutex_);
  if (completions_.empty() && !stopping_) {
    completion_available_.WaitWithTimeout(&mutex_, absl::Milliseconds(duration.count()));
  }
}

void Scheduler::CancelQueued() {
  absl::MutexLock lock(mutex_);
  std::size_t cancelled = 0;
  for (const std::unique_ptr<Lane>& lane : lanes_) {
    cancelled += lane->requests.size();
    lane->requests.clear();
  }
  queued_calls_ -= cancelled;
  outstanding_calls_ -= cancelled;
  work_available_.SignalAll();
}

void Scheduler::RunLane(Lane* lane) {
  for (;;) {
    js_runtime::ToolRequest request;
    bool stop = false;
    {
      absl::MutexLock lock(mutex_);
      while (!stopping_ && (lane->requests.empty() || active_calls_ >= kMaxActiveCalls)) {
        work_available_.Wait(&mutex_);
      }
      if (stopping_) {
        const std::size_t cancelled = lane->requests.size();
        lane->requests.clear();
        queued_calls_ -= cancelled;
        outstanding_calls_ -= cancelled;
        stop = true;
      } else {
        request = std::move(lane->requests.front());
        lane->requests.pop_front();
        --queued_calls_;
        ++active_calls_;
      }
    }
    if (stop) {
      lane->client.reset();
      return;
    }

    js_runtime::ToolCompletion completion = Process(request);

    {
      absl::MutexLock lock(mutex_);
      --active_calls_;
      completions_.push_back(std::move(completion));
      completion_available_.SignalAll();
      work_available_.SignalAll();
    }
  }
}

js_runtime::ToolCompletion Scheduler::Process(const js_runtime::ToolRequest& request) {
  const auto remaining = request.deadline - std::chrono::steady_clock::now();
  if (remaining <= std::chrono::steady_clock::duration::zero()) {
    return {request.run_id, request.id, false, nullptr, "tool deadline expired", "budget"};
  }
  if (trace_logger_ != nullptr) {
    trace_logger_->LogToolCall(request.run_id, request.id, request.server, request.tool, request.arguments);
  }
  auto result = catalog_.Call(request.server, request.tool, request.arguments, request.deadline);
  if (!result.ok()) {
    if (trace_logger_ != nullptr) {
      trace_logger_->LogToolFailure(request.run_id, request.id, request.server, request.tool, result.status());
    }
    return {request.run_id, request.id, false, nullptr, Bounded(std::string(result.status().message())), "downstream"};
  }
  auto normalized = NormalizeToolResult(*result);
  if (trace_logger_ != nullptr) {
    trace_logger_->LogToolResult(request.run_id, request.id, request.server, request.tool, *result, normalized);
  }
  if (!normalized.ok()) {
    return {request.run_id, request.id, false, nullptr, Bounded(std::string(normalized.status().message())),
            "normalization"};
  }
  return {request.run_id,
          request.id,
          normalized->ok,
          std::move(normalized->value),
          Bounded(normalized->error),
          normalized->error_category};
}

}  // namespace slop::mcp::gateway
