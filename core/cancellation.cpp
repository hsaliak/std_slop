#include "core/cancellation.h"

namespace slop {

void CancellationRequest::Cancel() {
  std::vector<std::function<void()>> to_run;
  {
    absl::MutexLock lock(&mu_);
    if (cancelled_) return;
    cancelled_ = true;
    to_run.swap(callbacks_);
  }
  for (auto& cb : to_run) {
    cb();
  }
}

bool CancellationRequest::IsCancelled() const {
  absl::ReaderMutexLock lock(&mu_);
  return cancelled_;
}

void CancellationRequest::RegisterCallback(std::function<void()> cb) {
  bool already_cancelled = false;
  {
    absl::MutexLock lock(&mu_);
    if (cancelled_) {
      already_cancelled = true;
    } else {
      callbacks_.push_back(std::move(cb));
    }
  }
  if (already_cancelled) {
    cb();
  }
}

}  // namespace slop
