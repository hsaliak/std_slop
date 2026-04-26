#include "rpc/execution_policy.h"

namespace slop::rpc::v1 {

void ApplyServerExecutionPolicy(slop::ToolExecutor& tool_executor, const ServerRuntimeConfig& server_config) {
  tool_executor.SetAskUserEnabled(!server_config.disable_ask_user);
  tool_executor.SetMaxSubqueryExecutionDepth(kRpcMaxExecutionDepth);
}

void ApplyServerExecutionPolicy(slop::InteractionEngine::Config& engine_config,
                                const ServerRuntimeConfig& server_config) {
  engine_config.allow_ask_user = !server_config.disable_ask_user;
  engine_config.max_subquery_execution_depth = kRpcMaxExecutionDepth;
}

}  // namespace slop::rpc::v1
