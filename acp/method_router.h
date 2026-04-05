
#ifndef SLOP_ACP_METHOD_ROUTER_H_
#define SLOP_ACP_METHOD_ROUTER_H_

#include <memory>
#include <string>

#include "acp/capabilities.h"
#include "acp/engine_adapter.h"
#include "acp/rpc_envelope.h"
#include "core/cancellation.h"
#include "core/database.h"
#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/mutex.h"

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
  DispatchOutcome HandleSessionCancel(const RpcRequest& request, const NegotiatedRuntimeOptions& state) const;
  DispatchOutcome HandleSessionPrompt(const RpcRequest& request, const NegotiatedRuntimeOptions& state, Database* db,
                                      const PromptExecutor& prompt_executor) const;

  std::shared_ptr<CancellationRequest> RegisterInFlightPrompt(const std::string& session_id) const;
  std::shared_ptr<CancellationRequest> FindInFlightPrompt(const std::string& session_id) const;
  void RemoveInFlightPrompt(const std::string& session_id) const;

  mutable absl::Mutex in_flight_mu_;
  mutable absl::flat_hash_map<std::string, std::shared_ptr<CancellationRequest>> in_flight_prompts_
      ABSL_GUARDED_BY(in_flight_mu_);
};

}  // namespace slop::acp

#endif  // SLOP_ACP_METHOD_ROUTER_H_