
#ifndef SLOP_ACP_SERVER_H_
#define SLOP_ACP_SERVER_H_

#include <istream>
#include <ostream>

#include "acp/method_router.h"

namespace slop { class Database; }
namespace slop::acp {

class Server {
 public:
  Server(std::istream* in, std::ostream* out, Database* db, PromptExecutor prompt_executor, MethodRouter router);

  // Processes requests from input until EOF.
  void Run();

 private:
  std::istream* in_;
  std::ostream* out_;
  NegotiatedRuntimeOptions state_;
  Database* db_;
  PromptExecutor prompt_executor_;
  MethodRouter router_;
};

int RunServer(std::istream* in, std::ostream* out, Database* db, PromptExecutor prompt_executor);

}  // namespace slop::acp

#endif  // SLOP_ACP_SERVER_H_