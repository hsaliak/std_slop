#include <algorithm>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "core/status_macros.h"
#include "tools/tool_executor.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandleUseSkill(const nlohmann::json& args) {
  if (!db_) {
    return absl::FailedPreconditionError("Database not initialized");
  }
  if (!args.is_object()) {
    return absl::InvalidArgumentError("Arguments must be a JSON object");
  }

  auto name = json_get<std::string>(args, "name");
  if (!name || name->empty()) {
    return absl::InvalidArgumentError("Missing mandatory field: name");
  }

  std::string action = "activate";
  if (auto action_arg = json_get<std::string>(args, "action")) {
    action = *action_arg;
  }
  if (action != "activate" && action != "deactivate") {
    return absl::InvalidArgumentError("INVALID_ARGUMENT: action must be 'activate' or 'deactivate'");
  }

  if (session_id_.empty()) {
    return absl::FailedPreconditionError("No active session");
  }

  // Match historical JS behavior: fail when current session row does not exist.
  ASSIGN_OR_RETURN(auto session_rows_json,
                   db_->Query("SELECT id, active_skills FROM sessions WHERE id = ?", {session_id_}));
  const auto session_rows = json_parse(session_rows_json);
  if (!session_rows.has_value() || !session_rows->is_array()) {
    return absl::InternalError("Invalid session lookup response");
  }
  if (session_rows->empty()) {
    return absl::FailedPreconditionError(absl::StrCat("Session not found: ", session_id_));
  }

  auto exists_or = db_->SkillExists(*name);
  if (!exists_or.ok()) return exists_or.status();
  if (!*exists_or) {
    return absl::NotFoundError(absl::StrCat("UNKNOWN_SKILL: ", *name));
  }

  ASSIGN_OR_RETURN(auto active_skills, db_->GetActiveSkills(session_id_));

  std::string prompt_patch;
  if (action == "activate") {
    if (std::find(active_skills.begin(), active_skills.end(), *name) == active_skills.end()) {
      active_skills.push_back(*name);
      RETURN_IF_ERROR(db_->IncrementSkillActivationCount(*name));
    }

    ASSIGN_OR_RETURN(auto skills, db_->GetSkills());
    for (const auto& skill : skills) {
      if (skill.name == *name && !skill.system_prompt_patch.empty()) {
        prompt_patch = absl::StrCat("\n\n", skill.system_prompt_patch);
        break;
      }
    }
  } else {
    active_skills.erase(std::remove(active_skills.begin(), active_skills.end(), *name), active_skills.end());
  }

  RETURN_IF_ERROR(db_->SetActiveSkills(session_id_, active_skills));
  InvalidateActiveSkillsCache();
  return absl::StrCat("Skill '", *name, "' ", (action == "activate" ? "activated" : "deactivated"), ".", prompt_patch);
}

}  // namespace slop
