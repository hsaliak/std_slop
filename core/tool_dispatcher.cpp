#include "core/tool_dispatcher.h"

#include <chrono>

#include "absl/log/log.h"

namespace slop {

ToolDispatcher::ToolDispatcher(ToolFunc executor_func) : executor_func_(std::move(executor_func)) {}

ToolDispatcher::~ToolDispatcher() {
  std::vector<std::thread> threads_to_join;
  {
    absl::MutexLock lock(&mu_);
    threads_to_join.reserve(threads_.size());
    for (auto& jt : threads_) {
      threads_to_join.push_back(std::move(jt.thread));
    }
    threads_.clear();
  }
  for (auto& thread : threads_to_join) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

// PruneThreads is called on every new submission to clean up resources from
// finished jobs. We join threads here rather than having them join themselves
// because a thread cannot join itself, and this approach avoids the complexity
// of a detached thread while ensuring that we don't leak thread handles.
// This also ensures that any resources held by the ToolJob are released
// promptly after completion.
void ToolDispatcher::PruneThreads(std::vector<std::thread>* threads_to_join) {
  for (auto it = threads_.begin(); it != threads_.end();) {
    if (it->job->IsReady()) {
      threads_to_join->push_back(std::move(it->thread));
      it = threads_.erase(it);
    } else {
      ++it;
    }
  }
}

std::shared_ptr<ToolJob> ToolDispatcher::Submit(const Call& call, std::shared_ptr<CancellationRequest> cancellation,
                                                 int64_t delay_ms) {
  auto job = std::make_shared<ToolJob>(call.id, call.name);
  auto task = [job, cancellation, this, call, delay_ms]() {
    if (delay_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    // The cancellation request is shared between the caller and this thread.
    // If the caller (or any other holder) triggers cancellation, this job will
    // see it either here or inside the executor_func_.
    if (cancellation && cancellation->IsCancelled()) {
      job->SetResult(absl::CancelledError("Cancelled"));
      return;
    }
    job->SetResult(executor_func_(call.name, call.args, cancellation));
  };

  std::vector<std::thread> threads_to_join;
  {
    absl::MutexLock lock(&mu_);
    threads_to_join.reserve(threads_.size());
    PruneThreads(&threads_to_join);
    threads_.push_back({std::thread(std::move(task)), job});
  }
  for (auto& thread : threads_to_join) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  return job;
}

std::vector<ToolDispatcher::Result> ToolDispatcher::Dispatch(const std::vector<Call>& calls,
                                                             std::shared_ptr<CancellationRequest> cancellation,
                                                             int throttle_seconds) {
  if (calls.empty()) return {};

  std::vector<std::shared_ptr<ToolJob>> jobs;
  jobs.reserve(calls.size());
  for (size_t i = 0; i < calls.size(); ++i) {
    jobs.push_back(Submit(calls[i], cancellation, static_cast<int64_t>(i) * throttle_seconds * 1000));
  }

  std::vector<Result> results;
  results.reserve(calls.size());
  for (auto& job : jobs) {
    Result res;
    res.id = job->id();
    res.name = job->name();
    res.output = job->Wait();
    results.push_back(std::move(res));
  }
  return results;
}

}  // namespace slop
