#include "core/tool_dispatcher.h"

#include <chrono>

#include "absl/log/log.h"

namespace slop {

ToolDispatcher::ToolDispatcher(ToolFunc executor_func, int num_threads)
    : executor_func_(std::move(executor_func)), num_threads_(num_threads) {
  for (int i = 0; i < num_threads_; ++i) {
    workers_.emplace_back(&ToolDispatcher::WorkerLoop, this);
  }
}

ToolDispatcher::~ToolDispatcher() {
  {
    absl::MutexLock lock(&mu_);
    stop_ = true;
  }
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

std::shared_ptr<ToolJob> ToolDispatcher::Submit(const Call& call, std::shared_ptr<CancellationRequest> cancellation) {
  auto job = std::make_shared<ToolJob>(call.id, call.name);
  auto task = [this, call, job, cancellation]() {
    if (cancellation && cancellation->IsCancelled()) {
      job->SetResult(absl::CancelledError("Cancelled"));
      return;
    }
    job->SetResult(executor_func_(call.name, call.args, cancellation));
  };
  {
    absl::MutexLock lock(&mu_);
    tasks_.emplace(std::move(task));
  }
  return job;
}

std::vector<ToolDispatcher::Result> ToolDispatcher::Dispatch(const std::vector<Call>& calls,
                                                             std::shared_ptr<CancellationRequest> cancellation) {
  if (calls.empty()) return {};

  std::vector<std::shared_ptr<ToolJob>> jobs;
  jobs.reserve(calls.size());
  for (const auto& call : calls) {
    jobs.push_back(Submit(call, cancellation));
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

void ToolDispatcher::WorkerLoop() {
  while (true) {
    std::function<void()> task;
    {
      absl::MutexLock lock(&mu_);
      auto condition = [this]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) { return stop_ || !tasks_.empty(); };
      mu_.Await(absl::Condition(&condition));

      if (stop_ && tasks_.empty()) return;

      task = std::move(tasks_.front());
      tasks_.pop();
    }
    task();
  }
}

}  // namespace slop
