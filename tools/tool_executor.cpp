#include "tools/tool_executor.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"

#include "core/database.h"
#include "core/json_utils.h"
#include "core/shell_util.h"
#include "core/status_macros.h"
#include "tools/tool_dispatcher.h"
#include "tools/common.h"
#include "interface/color.h"
#include "interface/renderer.h"
#include "interface/terminal.h"

namespace slop {

ToolExecutor::ToolExecutor(Database* db) : db_(db) { RegisterTools(); }

ToolExecutor::~ToolExecutor() = default;

void ToolExecutor::SetDispatcher(std::unique_ptr<ToolDispatcher> dispatcher) { dispatcher_ = std::move(dispatcher); }

void ToolExecutor::RegisterTool(const std::string& name, ToolHandler handler) {
  CHECK(!dispatch_map_.contains(name)) << "Duplicate tool registration: " << name;
  dispatch_map_[name] = std::move(handler);
}

std::vector<std::string> ToolExecutor::GetRegisteredToolNamesForTest() const {
  std::vector<std::string> names;
  names.reserve(dispatch_map_.size());
  for (const auto& [name, _] : dispatch_map_) {
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

void ToolExecutor::RegisterTools() {
  RegisterTool("query_db", [this](const nlohmann::json& args, auto) { return HandleQueryDb(args); });
  RegisterTool("read_file", [this](const nlohmann::json& args, auto) { return HandleReadFile(args); });
  RegisterTool("list_directory", [this](const nlohmann::json& args, auto) { return HandleListDirectory(args); });
  RegisterTool("describe_db", [this](const nlohmann::json& args, auto) { return HandleDescribeDb(args); });
  RegisterTool("grep", [this](const nlohmann::json& args, auto) { return HandleGrep(args); });
  RegisterTool("execute_bash", [this](const nlohmann::json& args, auto) { return HandleExecuteBash(args); });
  RegisterTool("patch_tool", [this](const nlohmann::json& args, auto) { return HandlePatchTool(args); });
  RegisterTool("write_file", [this](const nlohmann::json& args, auto) { return HandleWriteFile(args); });
  RegisterTool("read_scratchpad", [this](const nlohmann::json& args, auto) { return HandleReadScratchpad(args); });
  RegisterTool("write_scratchpad", [this](const nlohmann::json& args, auto) { return HandleWriteScratchpad(args); });
  RegisterTool("use_skill", [this](const nlohmann::json& args, auto) { return HandleUseSkill(args); });
  RegisterTool("git_create_staging_branch",
               [this](const nlohmann::json& args, auto) { return HandleGitCreateStagingBranch(args); });
  RegisterTool("git_commit_patch",
               [this](const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
                 return HandleGitCommitPatch(args, cancellation);
               });
  RegisterTool("git_format_patch_series",
               [this](const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
                 return HandleGitFormatPatchSeries(args, cancellation);
               });
  RegisterTool("git_reroll_patch",
               [this](const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
                 return HandleGitRerollPatch(args, cancellation);
               });
  RegisterTool("git_verify_series",
               [this](const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
                 return HandleGitVerifySeries(args, cancellation);
               });
  RegisterTool("git_finalize_series",
               [this](const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
                 return HandleGitFinalizeSeries(args, cancellation);
               });
  RegisterTool("ask_user", [this](const nlohmann::json& args, auto) -> absl::StatusOr<std::string> {
    std::string prompt_text = "Input required: ";
    if (auto p = json_get<std::string>(args, "prompt")) {
      prompt_text = *p;
    }

    while (true) {
      std::string response;
      if (ask_user_handler_) {
        response = ask_user_handler_(prompt_text);
      } else {
        std::cout << "\n" << ansi::Yellow << "Agent asks:\n" << ansi::Reset;
        slop::Renderer::Get().PrintMarkdown(prompt_text);
        response = slop::ReadLine("reply");
      }

      if (!absl::StartsWith(response, "/")) {
        return response;
      }

      std::cout << "\n"
                << ansi::Red << "Error: " << ansi::Reset
                << "/commands don't work in Q&A mode. Please provide a direct answer without using slash commands."
                << std::endl;
    }
  });
}

absl::StatusOr<std::string> ToolExecutor::Execute(const std::string& name, const nlohmann::json& args,
                                                  std::shared_ptr<CancellationRequest> cancellation) {
  if (IsDebugToolsEnabled()) {
    LOG(INFO) << "[tool_debug] Execute name=" << name << " args_keys=" << JsonKeys(args);
  }
  RETURN_IF_ERROR(ValidateSubqueryPolicy(name));

  auto it = dispatch_map_.find(name);
  if (it != dispatch_map_.end()) {
    auto res = it->second(args, cancellation);
    if (IsDebugToolsEnabled()) {
      LOG(INFO) << "[tool_debug] Execute direct name=" << name << " status=" << (res.ok() ? "ok" : "error")
                << " output_preview=" << (res.ok() ? TruncateForLog(*res) : TruncateForLog(res.status().ToString()));
    }
    if (res.ok() && db_) {
      (void)db_->IncrementToolCallCount(name);
    }
    return res;
  }

  return absl::NotFoundError(absl::StrCat("NOT_FOUND: Tool not found: ", name));
}

void ToolExecutor::InvalidateActiveSkillsCache() {
  absl::MutexLock lock(active_skills_mu_);
  active_skills_cache_valid_ = false;
  active_skills_cache_session_id_.clear();
  active_skills_cache_.clear();
  active_skills_cache_set_.clear();
}

void ToolExecutor::RefreshActiveSkillsCacheIfNeeded() {
  if (session_id_.empty() || !db_) {
    InvalidateActiveSkillsCache();
    return;
  }

  {
    absl::MutexLock lock(active_skills_mu_);
    if (active_skills_cache_valid_ && active_skills_cache_session_id_ == session_id_) return;
  }

  auto skills_or = db_->GetActiveSkills(session_id_);
  std::vector<std::string> active_skills = skills_or.ok() ? *skills_or : std::vector<std::string>{};
  absl::flat_hash_set<std::string> active_skill_set(active_skills.begin(), active_skills.end());

  absl::MutexLock lock(active_skills_mu_);
  active_skills_cache_valid_ = true;
  active_skills_cache_session_id_ = session_id_;
  active_skills_cache_ = std::move(active_skills);
  active_skills_cache_set_ = std::move(active_skill_set);
}

void ToolExecutor::SetSessionId(const std::string& session_id) {
  if (session_id_ != session_id) {
    session_id_ = session_id;
    InvalidateActiveSkillsCache();
  }
}

void ToolExecutor::SetMailMode(bool enabled) {
  mail_mode_ = enabled;
  if (db_) {
    (void)db_->Query(enabled ? "UPDATE settings SET mode = 'mail' WHERE id = 1"
                             : "UPDATE settings SET mode = 'standard' WHERE id = 1");
  }
}

void ToolExecutor::SetExecutionContext(ExecutionScope scope, int depth) { execution_context_ = {scope, depth}; }

absl::Status ToolExecutor::ValidateSubqueryPolicy(const std::string& tool_name) const {
  const auto [scope, depth] = execution_context_;
  if (depth > 1) {
    return absl::InvalidArgumentError(
        "Subquery policy violation: execution_depth must be <= 1 for llm_query specializations");
  }

  if (scope != ExecutionScope::kSubquery) {
    return absl::OkStatus();
  }

  if (tool_name == "llm_query" || absl::StartsWith(tool_name, "llm_tool_")) {
    return absl::InvalidArgumentError(absl::StrCat("Subquery policy violation: tool '", tool_name,
                                                   "' is not allowed in subquery scope"));
  }
  return absl::OkStatus();
}

bool ToolExecutor::IsSkillActive(const std::string& name) {
  RefreshActiveSkillsCacheIfNeeded();
  absl::MutexLock lock(active_skills_mu_);
  if (!active_skills_cache_valid_) return false;
  return active_skills_cache_set_.contains(name);
}

std::vector<std::string> ToolExecutor::GetActiveSkills() {
  RefreshActiveSkillsCacheIfNeeded();
  absl::MutexLock lock(active_skills_mu_);
  if (!active_skills_cache_valid_) {
    return {};
  }
  return active_skills_cache_;
}

absl::StatusOr<std::string> ToolExecutor::GetBaseBranch(const std::string& requested_base) {
  return ResolveBaseBranch(db_, requested_base);
}

}  // namespace slop
