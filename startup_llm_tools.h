
#ifndef SLOP_STARTUP_LLM_TOOLS_H_
#define SLOP_STARTUP_LLM_TOOLS_H_

#include <functional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "core/config.h"

namespace slop {

class Database;
class ToolExecutor;

using LlmQueryInvoker =
    std::function<absl::StatusOr<std::string>(const std::string& query, const std::vector<std::string>& skills)>;

// Registers config-defined llm_query specialization tools in both the database
// and tool executor.
//
// Phase 2 wiring: handlers currently forward query + specialization skill to
// the llm_query invoker. Additional specialization fields are consumed in
// later phases when QueryOptions are introduced.
absl::Status RegisterLlmToolSpecializations(Database* db, ToolExecutor* tool_executor,
                                            const std::vector<LlmToolSpecializationConfig>& configs,
                                            const std::vector<std::string>& active_skills,
                                            LlmQueryInvoker llm_query_invoker);

}  // namespace slop

#endif  // SLOP_STARTUP_LLM_TOOLS_H_
