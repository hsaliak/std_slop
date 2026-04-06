
#include "acp/server.h"

#include <algorithm>
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

  auto dispatch_prompt_in_worker = [&](const RpcRequest& prompt_request, std::shared_ptr<CancellationRequest> cancellation) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    workers_.push_back(WorkerHandle{std::thread([this, req = prompt_request, cancellation, done, &write_if_needed]() {
                                       auto parsed_or = ParseSessionPromptParams(req.params);
                                       if (!parsed_or.ok()) {
                                         write_if_needed(
                                             DispatchOutcome{true, MakeInvalidRequestResponse(req.id, parsed_or.status().message())});
                                         done->store(true, std::memory_order_release);
                                         return;
                                       }

                                       auto result_or = ExecuteSessionPrompt(db_, *parsed_or, prompt_executor_, cancellation);
                                       router_.RemoveInFlightPrompt(parsed_or->session_id, cancellation);

                                       DispatchOutcome outcome;
                                       outcome.has_response = req.id.has_value();
                                       if (!outcome.has_response) {
                                         done->store(true, std::memory_order_release);
                                         return;
                                       }
                                       if (!result_or.ok()) {
                                         if (result_or.status().code() == absl::StatusCode::kInternal) {
                                           outcome.response = MakeInternalErrorResponse(req.id, result_or.status().message());
                                         } else {
                                           outcome.response = MakeInvalidRequestResponse(req.id, result_or.status().message());
                                         }
                                         write_if_needed(outcome);
                                         done->store(true, std::memory_order_release);
                                         return;
                                       }
                                       outcome.response =
                                           nlohmann::json({{"jsonrpc", "2.0"}, {"id", *req.id}, {"result", *result_or}});
                                       write_if_needed(outcome);
                                       done->store(true, std::memory_order_release);
                                     }),
                               done});
  };

  auto reap_completed_workers = [&]() {
    for (auto it = workers_.begin(); it != workers_.end();) {
      if (it->done->load(std::memory_order_acquire)) {
        if (it->thread.joinable()) {
          it->thread.join();
        }
        it = workers_.erase(it);
      } else {
        ++it;
      }
    }
  };

  while (true) {
    reap_completed_workers();

    auto line = transport.ReadLine();
    if (!line.has_value()) {
      for (auto& worker : workers_) {
        if (worker.thread.joinable()) {
          worker.thread.join();
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

    if (request_or->method == "session/prompt") {
      if (!state_.initialized) {
        write_if_needed(DispatchOutcome{request_or->id.has_value(),
                                        request_or->id.has_value()
                                            ? MakeInvalidRequestResponse(request_or->id, "initialize_required")
                                            : nlohmann::json()});
        continue;
      }
      auto parsed_or = ParseSessionPromptParams(request_or->params);
      if (!parsed_or.ok()) {
        write_if_needed(DispatchOutcome{request_or->id.has_value(),
                                        request_or->id.has_value()
                                            ? MakeInvalidRequestResponse(request_or->id, parsed_or.status().message())
                                            : nlohmann::json()});
        continue;
      }

      auto cancellation_or = router_.RegisterInFlightPrompt(parsed_or->session_id);
      if (!cancellation_or.ok()) {
        write_if_needed(DispatchOutcome{request_or->id.has_value(),
                                        request_or->id.has_value()
                                            ? MakeInvalidRequestResponse(request_or->id, cancellation_or.status().message())
                                            : nlohmann::json()});
        continue;
      }

      dispatch_prompt_in_worker(*request_or, *cancellation_or);
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
