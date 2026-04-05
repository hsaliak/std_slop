
#include "acp/server.h"

#include <utility>

#include "acp/rpc_envelope.h"
#include "acp/transport_stdio.h"

namespace slop::acp {

Server::Server(std::istream* in, std::ostream* out, MethodRouter router)
    : in_(in), out_(out), router_(std::move(router)) {}

void Server::Run() {
  StdioTransport transport(in_, out_);
  while (true) {
    auto line = transport.ReadLine();
    if (!line.has_value()) {
      return;
    }

    auto request_or = ParseRpcRequest(*line);
    if (!request_or.ok()) {
      if (request_or.status().message() == "invalid_json") {
        transport.WriteJson(MakeParseErrorResponse());
      } else {
        transport.WriteJson(MakeInvalidRequestResponse(std::nullopt, "Invalid request"));
      }
      continue;
    }

    DispatchOutcome outcome = router_.Dispatch(*request_or);
    if (outcome.has_response) {
      transport.WriteJson(outcome.response);
    }
  }
}

int RunServer(std::istream* in, std::ostream* out) {
  MethodRouter router;
  Server server(in, out, std::move(router));
  server.Run();
  return 0;
}

}  // namespace slop::acp