
#ifndef SLOP_ACP_SERVER_H_
#define SLOP_ACP_SERVER_H_

#include <istream>
#include <ostream>

#include "acp/method_router.h"

namespace slop::acp {

class Server {
 public:
  Server(std::istream* in, std::ostream* out, MethodRouter router);

  // Processes requests from input until EOF.
  void Run();

 private:
  std::istream* in_;
  std::ostream* out_;
  MethodRouter router_;
};

int RunServer(std::istream* in, std::ostream* out);

}  // namespace slop::acp

#endif  // SLOP_ACP_SERVER_H_