
#ifndef SLOP_ACP_SERVER_H_
#define SLOP_ACP_SERVER_H_

#include <istream>
#include <ostream>
#include <atomic>
#include <thread>
#include <vector>

#include "acp/method_router.h"
#include "absl/synchronization/mutex.h"

namespace slop { class Database; }
namespace slop::acp {

class Server {
 public:
  Server(std::istream* in, std::ostream* out, Database* db, PromptExecutor prompt_executor);

  struct WorkerHandle {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };

  // Processes requests from input until EOF.
  void Run();

 private:
  std::istream* in_;
  std::ostream* out_;
  NegotiatedRuntimeOptions state_;
  Database* db_;
  PromptExecutor prompt_executor_;
  MethodRouter router_;
  absl::Mutex out_mu_;
  std::vector<WorkerHandle> workers_;
};

int RunServer(std::istream* in, std::ostream* out, Database* db, PromptExecutor prompt_executor);

}  // namespace slop::acp

#endif  // SLOP_ACP_SERVER_H_