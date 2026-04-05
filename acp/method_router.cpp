
#include "acp/method_router.h"

#include "acp/session_store.h"

namespace slop::acp {

DispatchOutcome MethodRouter::Dispatch(const RpcRequest& request, NegotiatedRuntimeOptions* state, Database* db) const {
  DispatchOutcome out;

  if (request.is_notification()) {
    return out;
  }

  if (request.method == "session/new") {
    return HandleSessionNew(request, *state, db);
  }

  if (request.method == "initialize") {
    return HandleInitialize(request, state);
  }

  if (request.method == "rpc.ping") {
    out.has_response = true;
    out.response = nlohmann::json({{"jsonrpc", "2.0"}, {"id", *request.id}, {"result", nlohmann::json::object()}});
    return out;
  }

  out.has_response = true;
  out.response = MakeMethodNotFoundResponse(request.id, request.method);
  return out;
}

DispatchOutcome MethodRouter::HandleInitialize(const RpcRequest& request, NegotiatedRuntimeOptions* state) const {
  DispatchOutcome out;
  out.has_response = true;

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
  out.has_response = true;

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

}  // namespace slop::acp