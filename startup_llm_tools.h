
#ifndef SLOP_STARTUP_LLM_TOOLS_H_
#define SLOP_STARTUP_LLM_TOOLS_H_

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

// Registers config-defined llm_query specialization tools in both the database
// and tool executor.
absl::Status RegisterLlmToolSpecializations(Database* db, ToolExecutor* tool_executor,
                                            const std::vector<LlmToolSpecializationConfig>& configs,
                                            const std::vector<std::string>& active_skills,
                                            LlmQueryInvoker llm_query_invoker);

}  // namespace slop

#endif  // SLOP_STARTUP_LLM_TOOLS_H_
