
#include "acp/method_router.h"

#include "absl/status/status.h"

namespace slop::acp {

DispatchOutcome MethodRouter::Dispatch(const RpcRequest& request, NegotiatedRuntimeOptions* state) const {
  DispatchOutcome out;

  if (request.is_notification()) {
    return out;
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


}  // namespace slop::acp