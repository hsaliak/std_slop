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

std::vector<ToolDispatcher::Result> ToolDispatcher::Dispatch(
    const std::vector<Call>& calls, std::shared_ptr<CancellationRequest> cancellation) {
  if (calls.empty()) return {};

  struct SharedState {
    absl::Mutex mu;
    int remaining ABSL_GUARDED_BY(mu);
    std::vector<Result> results ABSL_GUARDED_BY(mu);
  };
  auto state = std::make_shared<SharedState>();
  {
    absl::MutexLock lock(&state->mu);
    state->remaining = calls.size();
    state->results.resize(calls.size());
  }

  for (size_t i = 0; i < calls.size(); ++i) {
    const auto& call = calls[i];
    auto task = [this, i, call, state, cancellation]() {
      Result res;
      res.id = call.id;
      res.name = call.name;

      if (cancellation && cancellation->IsCancelled()) {
        res.output = absl::CancelledError("Cancelled");
      } else {
        res.output = executor_func_(call.name, call.args, cancellation);
      }

      absl::MutexLock lock(&state->mu);
      state->results[i] = std::move(res);
      state->remaining--;
    };

    absl::MutexLock lock(&mu_);
    tasks_.push(std::move(task));
  }

  // Wait for all tasks to complete
  {
    absl::MutexLock lock(&state->mu);
    auto all_done = [state]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(state->mu) {
      return state->remaining == 0;
    };
    state->mu.Await(absl::Condition(&all_done));
  }

  absl::MutexLock lock(&state->mu);
  return std::move(state->results);
}

void ToolDispatcher::WorkerLoop() {
  while (true) {
    std::function<void()> task;
    {
      absl::MutexLock lock(&mu_);
      auto condition = [this]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
        return stop_ || !tasks_.empty();
      };
      mu_.Await(absl::Condition(&condition));

      if (stop_ && tasks_.empty()) return;

      task = std::move(tasks_.front());
      tasks_.pop();
    }
    task();
  }
}

}  // namespace slop
