
#ifndef SLOP_ACP_METHOD_ROUTER_H_
#define SLOP_ACP_METHOD_ROUTER_H_

#include "acp/capabilities.h"
#include "acp/engine_adapter.h"
#include "acp/rpc_envelope.h"
#include "core/database.h"

namespace slop::acp {

struct DispatchOutcome {
  bool has_response = false;
  nlohmann::json response;
};


class MethodRouter {
 public:
  DispatchOutcome Dispatch(const RpcRequest& request, NegotiatedRuntimeOptions* state, Database* db,
                           const PromptExecutor& prompt_executor) const;

 private:
  DispatchOutcome HandleInitialize(const RpcRequest& request, NegotiatedRuntimeOptions* state) const;
  DispatchOutcome HandleSessionNew(const RpcRequest& request, const NegotiatedRuntimeOptions& state, Database* db) const;
  DispatchOutcome HandleSessionPrompt(const RpcRequest& request, const NegotiatedRuntimeOptions& state, Database* db,
                                      const PromptExecutor& prompt_executor) const;
};

}  // namespace slop::acp

#endif  // SLOP_ACP_METHOD_ROUTER_H_