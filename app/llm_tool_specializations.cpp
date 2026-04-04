#include "app/llm_tool_specializations.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "core/database.h"
#include "core/json_utils.h"
#include "tools/tool_executor.h"

namespace slop {

namespace {

absl::Status DeleteStaleSpecializationTools(Database* db,
                                            const std::vector<LlmToolSpecializationConfig>& configs) {
  if (configs.empty()) {
    return db->Execute("DELETE FROM tools WHERE name LIKE 'llm_tool_%'");
  }
  std::vector<std::string> placeholders(configs.size(), "?");
  std::vector<std::string> params;
  params.reserve(configs.size());
  for (const auto& cfg : configs) params.push_back(cfg.tool_name);
  const std::string sql = absl::StrCat("DELETE FROM tools WHERE name LIKE 'llm_tool_%' AND name NOT IN (",
                                       absl::StrJoin(placeholders, ", "), ")");
  const std::vector<std::string>& bind_params = params;
  return db->Execute(sql, bind_params);
}

}  // namespace

absl::Status ReconcileLlmSpecializationTools(Database* db,
                                             const std::vector<LlmToolSpecializationConfig>& configs) {
  if (db == nullptr) {
    return absl::InvalidArgumentError("db must not be null");
  }

  const absl::Status stale_cleanup_status = DeleteStaleSpecializationTools(db, configs);
  if (!stale_cleanup_status.ok()) {
    return stale_cleanup_status;
  }

  const std::string kLlmQuerySchema =
      R"({"type":"object","properties":{"query":{"type":"string","description":"The prompt to send to the LLM."}},"required":["query"]})";

  for (const auto& cfg : configs) {
    const std::string description = absl::StrCat(
        "Config-defined specialization of llm_query. session_id=", cfg.session_id, ", skill=", cfg.skill,
        cfg.context_window.has_value() ? absl::StrCat(", context_window=", *cfg.context_window) : "");

    const absl::Status db_status = db->RegisterTool({
        cfg.tool_name,
        description,
        kLlmQuerySchema,
        true,
    });
    if (!db_status.ok()) {
      return db_status;
    }
  }

  return absl::OkStatus();
}

absl::Status RegisterLlmSpecializationHandlers(ToolExecutor* tool_executor,
                                               const std::vector<LlmToolSpecializationConfig>& configs,
                                               const std::vector<std::string>& active_skills,
                                               LlmQueryInvoker llm_query_invoker) {
  if (tool_executor == nullptr) {
    return absl::InvalidArgumentError("tool_executor must not be null");
  }
  if (!llm_query_invoker) {
    return absl::InvalidArgumentError("llm_query_invoker must not be empty");
  }

  for (const auto& cfg : configs) {

    tool_executor->RegisterTool(
        cfg.tool_name,
        [llm_query_invoker, active_skills, specialization_skill = cfg.skill, context_window = cfg.context_window,
         session_id = cfg.session_id](const nlohmann::json& args,
                                      std::shared_ptr<slop::CancellationRequest>) -> absl::StatusOr<std::string> {
          auto query = slop::json_get<std::string>(args, "query");
          if (!query) {
            return absl::InvalidArgumentError("Missing 'query' argument");
          }

          std::vector<std::string> merged_skills = active_skills;
          if (!specialization_skill.empty() &&
              std::find(merged_skills.begin(), merged_skills.end(), specialization_skill) == merged_skills.end()) {
            merged_skills.push_back(specialization_skill);
          }

          LlmQueryOptions options;
          options.session_id = session_id;
          options.skill = specialization_skill;
          options.context_window = context_window;
          options.execution_scope = LlmQueryOptions::ExecutionScope::kSubquery;
          options.execution_depth = 1;
          return llm_query_invoker(*query, merged_skills, options);
        });
  }

  return absl::OkStatus();
}

}  // namespace slop
