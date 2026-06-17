#include "tools/tool_executor.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <vector>

#include "absl/container/flat_hash_set.h"
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
#include "js_bridge/interpreter.h"
#include "interface/color.h"
#include "interface/renderer.h"
#include "interface/terminal.h"
#include "tools/common.h"
#include "tools/tool_dispatcher.h"

namespace slop {

absl::StatusOr<std::string> HandleRunJsTool(
    const nlohmann::json& args,
    std::function<absl::StatusOr<std::string>(const std::string&, const nlohmann::json&)> tool_caller);

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
  RegisterTool("edit_tool", [this](const nlohmann::json& args, auto) { return HandleEditTool(args); });
  RegisterTool("write_file", [this](const nlohmann::json& args, auto) { return HandleWriteFile(args); });
  RegisterTool("persist_function", [this](const nlohmann::json& args, auto) { return HandlePersistFunction(args); });
  RegisterTool("read_scratchpad", [this](const nlohmann::json& args, auto) { return HandleReadScratchpad(args); });
  RegisterTool("write_scratchpad", [this](const nlohmann::json& args, auto) { return HandleWriteScratchpad(args); });
  RegisterTool("run_js", [this](const nlohmann::json& args, auto) { return HandleRunJs(args); });
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

namespace {

constexpr int kMaxPersistedFunctionCodeBytes = 64 * 1024;
constexpr int kMaxPersistedPreloadCodeBytes = 128 * 1024;

std::string PersistedFunctionInstallCode(const std::string& name, const std::string& code) {
  const std::string quoted_name = json_dump(name);
  const std::string quoted_code = json_dump(code);
  return absl::StrCat(
      "(function(){\n",
      "const __persisted_name = ", quoted_name, ";\n",
      "if (__persisted_name in globalThis) {\n",
      "  throw new TypeError('persisted function name collides with existing run_js global: ' + __persisted_name);\n",
      "}\n",
      "const __persisted_factory = new Function('return (' + ", quoted_code, " + '\\n);');\n",
      "const __persisted_function = __persisted_factory();\n",
      "if (typeof __persisted_function !== 'function' || __persisted_function.name !== __persisted_name) {\n",
      "  throw new TypeError('persisted code must be a named function ' + __persisted_name);\n",
      "}\n",
      "globalThis[__persisted_name] = __persisted_function;\n",
      "})();\n");
}

bool IsValidJsIdentifier(absl::string_view name) {
  if (name.empty()) return false;
  const auto is_identifier_start = [](unsigned char c) {
    return std::isalpha(c) || c == '_' || c == '$';
  };
  const auto is_identifier_part = [](unsigned char c) {
    return std::isalnum(c) || c == '_' || c == '$';
  };
  if (!is_identifier_start(static_cast<unsigned char>(name[0]))) return false;
  return std::all_of(name.begin() + 1, name.end(), [&](char c) {
    return is_identifier_part(static_cast<unsigned char>(c));
  });
}

bool IsReservedPersistedFunctionName(absl::string_view name) {
  static const absl::flat_hash_set<std::string> reserved_names = {
      "Array",          "Boolean",   "Date",      "Error",        "Function",   "JSON",
      "Math",           "Number",    "Object",    "Promise",      "Reflect",    "RegExp",
      "String",         "Symbol",    "SyntaxError", "TypeError",  "call_tool",  "dispatch",
      "edit_tool",      "execute_bash", "globalThis", "grep",     "help",       "input",
      "llm_query",      "list_directory", "persist_function", "query_db", "read_file", "run_js",
      "tools",          "undefined", "write_file",
  };
  return reserved_names.contains(std::string(name));
}

absl::Status ValidatePersistFunctionCode(const std::string& name, const std::string& code,
                                        const nlohmann::json& test_args,
                                        JsInterpreter::ToolCaller tool_caller) {
  const std::string validation_code = absl::StrCat(
      PersistedFunctionInstallCode(name, code), "\n",
      "const __persisted_tests = ", json_dump(test_args), ";\n",
      "const __persisted_function = globalThis[", json_dump(name), "];\n",
      "for (let i = 0; i < __persisted_tests.length; i++) {\n",
      "  __persisted_function(__persisted_tests[i]);\n",
      "}\n",
      "return true;\n");
  absl::StatusOr<nlohmann::json> result = RunJsForJson(validation_code, std::move(tool_caller));
  if (!result.ok()) {
    return absl::InvalidArgumentError(absl::StrCat("persist_function validation failed: ", result.status().message()));
  }
  return absl::OkStatus();
}

absl::Status ValidatePersistFunctionArgs(const nlohmann::json& args) {
  if (!args.is_object()) {
    return absl::InvalidArgumentError("persist_function args must be an object");
  }
  const std::optional<std::string> name = json_get<std::string>(args, "name");
  if (!name.has_value() || name->empty()) {
    return absl::InvalidArgumentError("persist_function requires non-empty string field name");
  }
  if (!IsValidJsIdentifier(*name)) {
    return absl::InvalidArgumentError("persist_function name must be a valid JavaScript identifier");
  }
  if (IsReservedPersistedFunctionName(*name)) {
    return absl::InvalidArgumentError("persist_function name collides with a built-in run_js global");
  }
  const std::optional<std::string> code = json_get<std::string>(args, "code");
  if (!code.has_value() || code->empty()) {
    return absl::InvalidArgumentError("persist_function requires non-empty string field code");
  }
  if (code->size() > kMaxPersistedFunctionCodeBytes) {
    return absl::InvalidArgumentError("persist_function code exceeds maximum size");
  }
  const absl::string_view trimmed_code = absl::StripAsciiWhitespace(*code);
  if (!absl::StartsWith(trimmed_code, absl::StrCat("function ", *name, "(")) || !absl::EndsWith(trimmed_code, "}")) {
    return absl::InvalidArgumentError(
        "persist_function code must be a single named function declaration matching name");
  }
  if (json_at(args, "description") != nullptr && !json_get<std::string>(args, "description").has_value()) {
    return absl::InvalidArgumentError("persist_function description must be a string");
  }
  if (json_at(args, "json_schema") != nullptr) {
    const std::optional<std::string> json_schema = json_get<std::string>(args, "json_schema");
    if (!json_schema.has_value()) {
      return absl::InvalidArgumentError("persist_function json_schema must be a string");
    }
    if (!json_schema->empty()) {
      const std::optional<nlohmann::json> parsed_schema = json_parse(*json_schema);
      if (!parsed_schema.has_value() || !parsed_schema->is_object()) {
        return absl::InvalidArgumentError("persist_function json_schema must be a JSON object string");
      }
    }
  }
  const nlohmann::json* test_args = json_at(args, "test_args");
  if (test_args != nullptr && !test_args->is_array()) {
    return absl::InvalidArgumentError("persist_function test_args must be an array");
  }
  return absl::OkStatus();
}

}  // namespace

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

absl::StatusOr<std::string> ToolExecutor::HandlePersistFunction(const nlohmann::json& args) {
  RETURN_IF_ERROR(ValidatePersistFunctionArgs(args));
  if (db_ == nullptr) {
    return absl::FailedPreconditionError("Database not initialized");
  }
  const std::string name = *json_get<std::string>(args, "name");
  const std::string code = *json_get<std::string>(args, "code");
  const std::string description = json_get<std::string>(args, "description").value_or("");
  const std::string json_schema = json_get<std::string>(args, "json_schema").value_or("");
  const nlohmann::json* test_args = json_at(args, "test_args");

  ASSIGN_OR_RETURN(const std::string existing_rows_json,
                   db_->Query("SELECT name, code FROM js_functions WHERE name != ? ORDER BY name", {name}));
  size_t persisted_preload_size = PersistedFunctionInstallCode(name, code).size();
  if (std::optional<nlohmann::json> existing_rows = json_parse(existing_rows_json);
      existing_rows.has_value() && existing_rows->is_array()) {
    for (const auto& row : *existing_rows) {
      const std::optional<std::string> existing_name = json_get<std::string>(row, "name");
      const std::optional<std::string> existing_code = json_get<std::string>(row, "code");
      if (existing_name.has_value() && existing_code.has_value()) {
        persisted_preload_size += PersistedFunctionInstallCode(*existing_name, *existing_code).size();
      }
    }
  }
  if (persisted_preload_size > kMaxPersistedPreloadCodeBytes) {
    return absl::InvalidArgumentError("persist_function would exceed persisted preload size limit");
  }

  RETURN_IF_ERROR(ValidatePersistFunctionCode(
      name, code, test_args == nullptr ? nlohmann::json::array() : *test_args,
      [this](const std::string& tool_name, const nlohmann::json& tool_args) -> absl::StatusOr<std::string> {
        if (tool_name == "run_js" || tool_name == "persist_function") {
          return absl::InvalidArgumentError(
              absl::StrCat("persist_function validation cannot invoke tool '", tool_name, "'"));
        }
        if (db_ != nullptr) {
          ASSIGN_OR_RETURN(bool is_callable, db_->IsRunJsCallableTool(tool_name));
          if (!is_callable) {
            return absl::InvalidArgumentError(
                absl::StrCat("tool '", tool_name, "' is not callable from persist_function validation"));
          }
        }
        return Execute(tool_name, tool_args);
      }));

  RETURN_IF_ERROR(db_->Execute(
      "INSERT OR REPLACE INTO js_functions(name, code, description, json_schema) VALUES (?, ?, ?, ?)", name, code,
      description, json_schema));
  return json_dump(nlohmann::json{{"name", name}, {"persisted", true}});
}

absl::StatusOr<std::string> ToolExecutor::HandleRunJs(const nlohmann::json& args) {
  nlohmann::json run_args = args;
  if (db_ != nullptr) {
    ASSIGN_OR_RETURN(const std::string rows_json, db_->Query("SELECT name, code FROM js_functions ORDER BY name"));
    if (std::optional<nlohmann::json> rows = json_parse(rows_json); rows.has_value() && rows->is_array()) {
      std::string persisted_code;
      for (const auto& row : *rows) {
        const std::optional<std::string> name = json_get<std::string>(row, "name");
        const std::optional<std::string> code = json_get<std::string>(row, "code");
        if (name.has_value() && code.has_value() && !code->empty()) {
          absl::StrAppend(&persisted_code, PersistedFunctionInstallCode(*name, *code));
        }
      }
      if (!persisted_code.empty()) {
        const std::optional<std::string> code = json_get<std::string>(args, "code");
        if (code.has_value()) {
          run_args["code"] = absl::StrCat(persisted_code, *code);
        }
      }
    }
  }

  return HandleRunJsTool(
      run_args, [this](const std::string& tool_name, const nlohmann::json& tool_args) -> absl::StatusOr<std::string> {
        if (tool_name == "run_js") {
          return absl::InvalidArgumentError("run_js bridge cannot recursively invoke run_js");
        }
        if (db_ != nullptr) {
          ASSIGN_OR_RETURN(bool is_callable, db_->IsRunJsCallableTool(tool_name));
          if (!is_callable) {
            return absl::InvalidArgumentError(
                absl::StrCat("tool '", tool_name, "' is not callable from run_js"));
          }
        }
        return Execute(tool_name, tool_args, nullptr);
      });
}

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
    return absl::InvalidArgumentError(
        absl::StrCat("Subquery policy violation: tool '", tool_name, "' is not allowed in subquery scope"));
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
