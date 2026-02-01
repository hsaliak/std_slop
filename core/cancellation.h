#ifndef CORE_CANCELLATION_H_
#define CORE_CANCELLATION_H_

#include <atomic>
#include <functional>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"

namespace slop {

class CancellationRequest {
 public:
  CancellationRequest() = default;

  // Trigger cancellation and run all registered callbacks.
  void Cancel();

  // Returns true if cancellation has been requested.
  bool IsCancelled() const;

  // Registers a callback to be run when Cancel() is called.
  // If Cancel() has already been called, the callback is run immediately.
  void RegisterCallback(std::function<void()> cb);

 private:
  mutable absl::Mutex mu_;
  bool cancelled_ ABSL_GUARDED_BY(mu_) = false;
  std::vector<std::function<void()>> callbacks_ ABSL_GUARDED_BY(mu_);
};

}  // namespace slop

#endif  // CORE_CANCELLATION_H_
