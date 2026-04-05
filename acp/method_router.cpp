
#include "acp/method_router.h"

namespace slop::acp {

DispatchOutcome MethodRouter::Dispatch(const RpcRequest& request) const {
  DispatchOutcome out;

  if (request.is_notification()) {
    return out;
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

}  // namespace slop::acp