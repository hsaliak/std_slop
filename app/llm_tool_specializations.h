
#ifndef SLOP_APP_LLM_TOOL_SPECIALIZATIONS_H_
#define SLOP_APP_LLM_TOOL_SPECIALIZATIONS_H_

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "core/config.h"

namespace slop {

class Database;
class ToolExecutor;

struct LlmQueryOptions {
  enum class ExecutionScope {
    kRoot,
    kSubquery,
  };

  std::string session_id = "query";
  std::optional<std::string> skill;
  std::optional<int> context_window;
  ExecutionScope execution_scope = ExecutionScope::kRoot;
  int execution_depth = 0;
};

using LlmQueryInvoker = std::function<absl::StatusOr<std::string>(const std::string& query,
                                                                  const std::vector<std::string>& active_skills,
                                                                  const LlmQueryOptions& options)>;

// Reconciles llm specialization tool records in the database.
absl::Status ReconcileLlmSpecializationTools(Database* db,
                                             const std::vector<LlmToolSpecializationConfig>& configs);

// Registers llm specialization handlers in the tool executor.
absl::Status RegisterLlmSpecializationHandlers(ToolExecutor* tool_executor,
                                               const std::vector<LlmToolSpecializationConfig>& configs,
                                               const std::vector<std::string>& active_skills,
                                               LlmQueryInvoker llm_query_invoker);

}  // namespace slop

#endif  // SLOP_APP_LLM_TOOL_SPECIALIZATIONS_H_
