
#ifndef SLOP_ACP_METHOD_ROUTER_H_
#define SLOP_ACP_METHOD_ROUTER_H_

#include "acp/rpc_envelope.h"

namespace slop::acp {

struct DispatchOutcome {
  bool has_response = false;
  nlohmann::json response;
};

class MethodRouter {
 public:
  DispatchOutcome Dispatch(const RpcRequest& request) const;
};

}  // namespace slop::acp

#endif  // SLOP_ACP_METHOD_ROUTER_H_