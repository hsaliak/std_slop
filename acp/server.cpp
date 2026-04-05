
#include "acp/server.h"

#include <utility>

#include "acp/rpc_envelope.h"
#include "acp/transport_stdio.h"

namespace slop::acp {

Server::Server(std::istream* in, std::ostream* out, Database* db, PromptExecutor prompt_executor)
    : in_(in), out_(out), db_(db), prompt_executor_(std::move(prompt_executor)) {}

void Server::Run() {
  StdioTransport transport(in_, out_);
  auto write_if_needed = [&](const DispatchOutcome& outcome) {
    if (!outcome.has_response) {
      return;
    }
    absl::MutexLock lock(out_mu_);
    transport.WriteJson(outcome.response);
  };

  auto dispatch_in_worker = [&](RpcRequest request) {
    workers_.emplace_back([this, req = std::move(request), &write_if_needed]() mutable {
      DispatchOutcome outcome = router_.Dispatch(req, &state_, db_, prompt_executor_);
      write_if_needed(outcome);
    });
  };

  while (true) {
    auto line = transport.ReadLine();
    if (!line.has_value()) {
      for (auto& worker : workers_) {
        if (worker.joinable()) {
          worker.join();
        }
      }
      return;
    }

    auto request_or = ParseRpcRequest(*line);
    if (!request_or.ok()) {
      absl::MutexLock lock(out_mu_);
      if (request_or.status().message() == "invalid_json") {
        transport.WriteJson(MakeParseErrorResponse());
      } else {
        transport.WriteJson(MakeInvalidRequestResponse(std::nullopt, "Invalid request"));
      }
      continue;
    }

    if (request_or->method == "session/prompt" || request_or->method == "session/cancel") {
      dispatch_in_worker(std::move(*request_or));
      continue;
    }

    DispatchOutcome outcome = router_.Dispatch(*request_or, &state_, db_, prompt_executor_);
    write_if_needed(outcome);
  }
}

int RunServer(std::istream* in, std::ostream* out, Database* db, PromptExecutor prompt_executor) {
  Server server(in, out, db, std::move(prompt_executor));
  server.Run();
  return 0;
}

}  // namespace slop::acp
