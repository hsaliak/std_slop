#ifndef SLOP_INTERFACE_ANIMATOR_H_
#define SLOP_INTERFACE_ANIMATOR_H_

#include <atomic>
#include <thread>

namespace slop {

class AsyncAnimator {
 public:
  AsyncAnimator();
  ~AsyncAnimator();

  void Start();
  void Stop();

 private:
  void RenderLoop();

  std::thread thread_;
  std::atomic<bool> is_running_;
};

}  // namespace slop

#endif  // SLOP_INTERFACE_ANIMATOR_H_
