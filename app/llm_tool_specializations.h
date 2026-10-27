
#ifndef SLOP_APP_LLM_TOOL_SPECIALIZATIONS_H_
#define SLOP_APP_LLM_TOOL_SPECIALIZATIONS_H_

#include <functional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "core/config.h"
#include "core/llm_query_options.h"
#include "nlohmann/json.hpp"

namespace slop {

class Database;
class ToolExecutor;

using LlmQueryInvoker = std::function<absl::StatusOr<std::string>(const std::string& query,
                                                                  const std::vector<std::string>& active_skills,
                                                                  const LlmQueryOptions& options)>;

// Validates the shared query argument before invoking a model.
absl::StatusOr<std::string> ParseLlmQueryArgument(const nlohmann::json& args);

// Registers the default llm_query handler with the supplied active skills.
absl::Status RegisterLlmQueryHandler(ToolExecutor* tool_executor, const std::vector<std::string>& active_skills,
                                     LlmQueryInvoker llm_query_invoker);

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
