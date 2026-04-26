#ifndef SLOP_RPC_EXECUTION_POLICY_H_
#define SLOP_RPC_EXECUTION_POLICY_H_

#include "interface/interaction_engine.h"
#include "rpc/server_config.h"
#include "tools/tool_executor.h"

namespace slop::rpc::v1 {

constexpr int kRpcMaxExecutionDepth = 1;

void ApplyServerExecutionPolicy(slop::ToolExecutor& tool_executor, const ServerRuntimeConfig& server_config);

void ApplyServerExecutionPolicy(slop::InteractionEngine::Config& engine_config,
                                const ServerRuntimeConfig& server_config);

}  // namespace slop::rpc::v1

#endif  // SLOP_RPC_EXECUTION_POLICY_H_
