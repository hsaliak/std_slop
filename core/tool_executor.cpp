#include "core/tool_executor.h"

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
#include "core/shell_util.h"
#include "core/status_macros.h"
#include "core/tool_dispatcher.h"
#include "core/tools/common.h"
#include "interface/color.h"
#include "interface/renderer.h"
#include "interface/terminal.h"
#include "core/json_utils.h"

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
  RegisterTool("parse_tool_rows", [this](const nlohmann::json& args, auto) { return HandleParseToolRows(args); });
  RegisterTool("use_skill", [this](const nlohmann::json& args, auto) { return HandleUseSkill(args); });
  RegisterTool("git_create_staging_branch",
               [this](const nlohmann::json& args, auto) { return HandleGitCreateStagingBranch(args); });
  RegisterTool("git_commit_patch", [this](const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
    return HandleGitCommitPatch(args, cancellation);
  });
  RegisterTool("git_format_patch_series",
               [this](const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
                 return HandleGitFormatPatchSeries(args, cancellation);
               });
  RegisterTool("git_reroll_patch", [this](const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
    return HandleGitRerollPatch(args, cancellation);
  });
  RegisterTool("git_finalize_series",
               [this](const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
                 return HandleGitFinalizeSeries(args, cancellation);
               });
  RegisterTool("grep_tool", [this](const nlohmann::json& args,
                                    std::shared_ptr<CancellationRequest> cancellation)
                                 -> absl::StatusOr<std::string> {
    if (!args.is_object() || !json_get<std::string>(args, "pattern")) {
      return absl::InvalidArgumentError("Missing mandatory field: pattern");
    }
    nlohmann::json simplified = {
        {"pattern", *json_get<std::string>(args, "pattern")},
    };
    if (auto path = json_get<std::string>(args, "path")) {
      simplified["path"] = *path;
    } else if (auto paths = json_get<std::string>(args, "paths")) {
      simplified["path"] = *paths;
    }
    if (auto context = json_get<int>(args, "context")) simplified["context"] = *context;
    if (auto limit = json_get<int>(args, "limit")) simplified["limit"] = *limit;
    if (auto include_ignored = json_get<bool>(args, "include_ignored")) simplified["include_ignored"] = *include_ignored;
    if (auto ignore = json_get<std::string>(args, "ignore")) simplified["ignore"] = *ignore;
    // Delegate to canonical grep implementation (currently JS-backed) to avoid behavior drift.
    return Execute("grep", simplified, cancellation);
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

void ToolExecutor::SetSessionId(const std::string& session_id) { session_id_ = session_id; }

void ToolExecutor::SetMailMode(bool enabled) {
  mail_mode_ = enabled;
  if (db_) {
    (void)db_->Query(enabled ? "UPDATE settings SET mode = 'mail' WHERE id = 1"
                             : "UPDATE settings SET mode = 'standard' WHERE id = 1");
  }
}

bool ToolExecutor::IsSkillActive(const std::string& name) {
  auto active = GetActiveSkills();
  return std::any_of(active.begin(), active.end(), [&name](const std::string& s) { return s == name; });
}

std::vector<std::string> ToolExecutor::GetActiveSkills() {
  if (session_id_.empty() || !db_) return {};
  auto skills_or = db_->GetActiveSkills(session_id_);
  if (skills_or.ok()) {
    return *skills_or;
  }
  return {};
}

absl::StatusOr<std::string> ToolExecutor::GetBaseBranch(const std::string& requested_base) {
  return ResolveBaseBranch(db_, requested_base);
}

}  // namespace slop
























































