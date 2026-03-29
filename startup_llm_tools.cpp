#include "startup_llm_tools.h"

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
    return db->Execute("DELETE FROM tools WHERE name LIKE 'llm_tool.%'");
  }
  std::vector<std::string> placeholders(configs.size(), "?");
  std::vector<std::string> params;
  params.reserve(configs.size());
  for (const auto& cfg : configs) params.push_back(cfg.tool_name);
  const std::string sql = absl::StrCat("DELETE FROM tools WHERE name LIKE 'llm_tool.%' AND name NOT IN (",
                                       absl::StrJoin(placeholders, ", "), ")");
  const std::vector<std::string>& bind_params = params;
  return db->Execute(sql, bind_params);
}

}  // namespace

absl::Status RegisterLlmToolSpecializations(Database* db, ToolExecutor* tool_executor,
                                            const std::vector<LlmToolSpecializationConfig>& configs,
                                            const std::vector<std::string>& active_skills,
                                            LlmQueryInvoker llm_query_invoker) {
  if (db == nullptr) {
    return absl::InvalidArgumentError("db must not be null");
  }
  if (tool_executor == nullptr) {
    return absl::InvalidArgumentError("tool_executor must not be null");
  }
  if (!llm_query_invoker) {
    return absl::InvalidArgumentError("llm_query_invoker must not be empty");
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

    tool_executor->RegisterTool(
        cfg.tool_name,
        [llm_query_invoker, active_skills, specialization_skill = cfg.skill](
            const nlohmann::json& args,
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

          return llm_query_invoker(*query, merged_skills);
        });
  }

  return absl::OkStatus();
}

}  // namespace slop
