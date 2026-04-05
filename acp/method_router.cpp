
#include "acp/method_router.h"

#include <chrono>
#include <thread>
#include "acp/engine_adapter.h"
#include "acp/session_store.h"
#include "absl/status/status.h"

namespace slop::acp {

std::shared_ptr<CancellationRequest> MethodRouter::RegisterInFlightPrompt(const std::string& session_id) const {
  auto cancellation = std::make_shared<CancellationRequest>();
  absl::MutexLock lock(in_flight_mu_);
  in_flight_prompts_[session_id] = cancellation;
  return cancellation;
}

std::shared_ptr<CancellationRequest> MethodRouter::FindInFlightPrompt(const std::string& session_id) const {
  absl::MutexLock lock(in_flight_mu_);
  auto it = in_flight_prompts_.find(session_id);
  if (it == in_flight_prompts_.end()) {
    return nullptr;
  }
  return it->second;
}

void MethodRouter::RemoveInFlightPrompt(const std::string& session_id) const {
  absl::MutexLock lock(in_flight_mu_);
  in_flight_prompts_.erase(session_id);
}

DispatchOutcome MethodRouter::Dispatch(const RpcRequest& request, NegotiatedRuntimeOptions* state, Database* db,
                                       const PromptExecutor& prompt_executor) const {
  DispatchOutcome out;
  if (request.method == "initialize") {
    return HandleInitialize(request, state);
  }
  if (request.method == "session/new") {
    return HandleSessionNew(request, *state, db);
  }
  if (request.method == "session/cancel") {
    return HandleSessionCancel(request, *state);
  }
  if (request.method == "session/prompt") {
    return HandleSessionPrompt(request, *state, db, prompt_executor);
  }

  if (!request.id.has_value()) {
    return out;
  }
  out.has_response = true;
  out.response = MakeMethodNotFoundResponse(request.id, request.method);
  return out;
}

DispatchOutcome MethodRouter::HandleInitialize(const RpcRequest& request, NegotiatedRuntimeOptions* state) const {
  DispatchOutcome out;
  out.has_response = request.id.has_value();
  if (!out.has_response) {
    return out;
  }

  auto parsed_or = ParseInitializeParams(request.params);
  if (!parsed_or.ok()) {
    out.response = MakeInvalidRequestResponse(request.id, parsed_or.status().message());
    return out;
  }

  ApplyInitializeRequest(*parsed_or, state);

  out.response = nlohmann::json({{"jsonrpc", "2.0"}, {"id", *request.id}, {"result", BuildInitializeResult()}});
  return out;
}

DispatchOutcome MethodRouter::HandleSessionNew(const RpcRequest& request, const NegotiatedRuntimeOptions& state,
                                               Database* db) const {
  DispatchOutcome out;
  out.has_response = request.id.has_value();
  if (!out.has_response) {
    return out;
  }

  if (!state.initialized) {
    out.response = MakeInvalidRequestResponse(request.id, "initialize_required");
    return out;
  }

  auto parsed_or = ParseSessionNewParams(request.params);
  if (!parsed_or.ok()) {
    out.response = MakeInvalidRequestResponse(request.id, parsed_or.status().message());
    return out;
  }

  auto session_id_or = CreateSession(db, *parsed_or);
  if (!session_id_or.ok()) {
    out.response = MakeInvalidRequestResponse(request.id, session_id_or.status().message());
    return out;
  }
  out.response = nlohmann::json({{"jsonrpc", "2.0"}, {"id", *request.id}, {"result", MakeSessionNewResult(*session_id_or)}});
  return out;
}

DispatchOutcome MethodRouter::HandleSessionCancel(const RpcRequest& request, const NegotiatedRuntimeOptions& state) const {
  DispatchOutcome out;
  out.has_response = request.id.has_value();

  if (!state.initialized) {
    if (out.has_response) {
      out.response = MakeInvalidRequestResponse(request.id, "initialize_required");
    }
    return out;
  }

  auto parsed_or = ParseSessionCancelParams(request.params);
  if (!parsed_or.ok()) {
    if (out.has_response) {
      out.response = MakeInvalidRequestResponse(request.id, parsed_or.status().message());
    }
    return out;
  }

  std::shared_ptr<CancellationRequest> cancellation;
  for (int attempt = 0; attempt < 100; ++attempt) {
    cancellation = FindInFlightPrompt(parsed_or->session_id);
    if (cancellation) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!cancellation) {
    if (out.has_response) {
      out.response = MakeInvalidRequestResponse(request.id, "session_cancel_request_not_found");
    }
    return out;
  }

  cancellation->Cancel();
  if (out.has_response) {
    out.response = nlohmann::json(
        {{"jsonrpc", "2.0"}, {"id", *request.id}, {"result", MakeSessionCancelResult(parsed_or->session_id)}});
  }
  return out;
}

DispatchOutcome MethodRouter::HandleSessionPrompt(const RpcRequest& request, const NegotiatedRuntimeOptions& state,
                                                  Database* db, const PromptExecutor& prompt_executor) const {
  DispatchOutcome out;
  out.has_response = request.id.has_value();
  if (!out.has_response) {
    return out;
  }

  if (!state.initialized) {
    out.response = MakeInvalidRequestResponse(request.id, "initialize_required");
    return out;
  }

  auto parsed_or = ParseSessionPromptParams(request.params);
  if (!parsed_or.ok()) {
    out.response = MakeInvalidRequestResponse(request.id, parsed_or.status().message());
    return out;
  }

  auto cancellation = RegisterInFlightPrompt(parsed_or->session_id);
  auto result_or = ExecuteSessionPrompt(db, *parsed_or, prompt_executor, cancellation);
  RemoveInFlightPrompt(parsed_or->session_id);

  if (!result_or.ok()) {
    if (result_or.status().code() == absl::StatusCode::kInternal) {
      out.response = MakeInternalErrorResponse(request.id, result_or.status().message());
      return out;
    }
    out.response = MakeInvalidRequestResponse(request.id, result_or.status().message());
    return out;
  }

  out.response = nlohmann::json({{"jsonrpc", "2.0"}, {"id", *request.id}, {"result", *result_or}});
  return out;
}

}  // namespace slop::acp
