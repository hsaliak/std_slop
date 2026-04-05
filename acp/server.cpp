
#include "acp/server.h"

#include <utility>

#include "acp/rpc_envelope.h"
#include "acp/transport_stdio.h"

namespace slop::acp {

Server::Server(std::istream* in, std::ostream* out, Database* db, PromptExecutor prompt_executor, MethodRouter router)
    : in_(in), out_(out), db_(db), prompt_executor_(std::move(prompt_executor)), router_(std::move(router)) {}

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

    DispatchOutcome outcome = router_.Dispatch(*request_or, &state_, db_, prompt_executor_);
    if (outcome.has_response) {
      transport.WriteJson(outcome.response);
    }
  }
}

int RunServer(std::istream* in, std::ostream* out, Database* db, PromptExecutor prompt_executor) {
  MethodRouter router;
  Server server(in, out, db, std::move(prompt_executor), std::move(router));
  server.Run();
  return 0;
}

}  // namespace slop::acp