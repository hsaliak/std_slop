#include "core/tool_executor.h"
#include "lua-bridge/interpreter.h"
#include "core/lua_bridge_util.h"
#include "core/preamble_data.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <unordered_set>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/substitute.h"

#include "core/shell_util.h"
namespace slop {

ToolExecutor::ToolExecutor(Database* db) : db_(db) {
  dispatch_map_["read_file"] = [this](const nlohmann::json& args, auto) {
    return ReadFile(args.get<ReadFileRequest>());
  };
  dispatch_map_["write_file"] = [this](const nlohmann::json& args, auto) {
    return WriteFile(args.get<WriteFileRequest>());
  };
  dispatch_map_["apply_patch"] = [this](const nlohmann::json& args, auto) {
    return ApplyPatch(args.get<ApplyPatchRequest>());
  };
  dispatch_map_["grep_tool"] = [this](const nlohmann::json& args, auto cancellation) {
    return RunLuaTool("grep_tool", args, cancellation);
  };
  dispatch_map_["git_grep_tool"] = [this](const nlohmann::json& args, auto cancellation) {
    return RunLuaTool("git_grep_tool", args, cancellation);
  };
  dispatch_map_["execute_bash"] = [this](const nlohmann::json& args, auto cancellation) {
    return ExecuteBash(args.get<ExecuteBashRequest>(), cancellation);
  };
  dispatch_map_["query_db"] = [this](const nlohmann::json& args, auto) {
    return QueryDb(args.get<QueryDbRequest>());
  };
  dispatch_map_["save_memo"] = [this](const nlohmann::json& args, auto cancellation) {
    return RunLuaTool("save_memo", args, cancellation);
  };
  dispatch_map_["retrieve_memos"] = [this](const nlohmann::json& args, auto cancellation) {
    return RunLuaTool("retrieve_memos", args, cancellation);
  };
  dispatch_map_["list_directory"] = [this](const nlohmann::json& args, auto cancellation) {
    return ListDirectory(args.get<ListDirectoryRequest>(), cancellation);
  };
  dispatch_map_["manage_scratchpad"] = [this](const nlohmann::json& args, auto cancellation) {
    return RunLuaTool("manage_scratchpad", args, cancellation);
  };
  dispatch_map_["describe_db"] = [this](const nlohmann::json&, auto) { return DescribeDb(); };
  dispatch_map_["use_skill"] = [this](const nlohmann::json& args, auto) {
    return UseSkill(args.get<UseSkillRequest>());
  };
  dispatch_map_["search_code"] = [this](const nlohmann::json& args, auto cancellation) {
    return RunLuaTool("search_code", args, cancellation);
  };

  lua_tools_ = {"grep_tool",
                "git_grep_tool",
                "search_code",
                "save_memo",
                "retrieve_memos",
                "manage_scratchpad",
                "git_branch_staging",
                "git_commit_patch",
                "git_reroll_patch",
                "git_verify_series",
                "git_format_patch_series",
                "git_finalize_series"};

  dispatch_map_["git_branch_staging"] = [this](const nlohmann::json& args, auto cancellation) {
    return RunLuaTool("git_branch_staging", args, cancellation);
  };
  dispatch_map_["git_commit_patch"] = [this](const nlohmann::json& args, auto cancellation) {
    return RunLuaTool("git_commit_patch", args, cancellation);
  };
  dispatch_map_["git_format_patch_series"] = [this](const nlohmann::json& args, auto cancellation) {
    return RunLuaTool("git_format_patch_series", args, cancellation);
  };
  dispatch_map_["git_finalize_series"] = [this](const nlohmann::json& args, auto cancellation) {
    return RunLuaTool("git_finalize_series", args, cancellation);
  };
  dispatch_map_["git_verify_series"] = [this](const nlohmann::json& args, auto cancellation) {
    return RunLuaTool("git_verify_series", args, cancellation);
  };
  dispatch_map_["git_reroll_patch"] = [this](const nlohmann::json& args, auto cancellation) {
    return RunLuaTool("git_reroll_patch", args, cancellation);
  };
  dispatch_map_["run_lua"] = [this](const nlohmann::json& args, auto cancellation) {
    return RunLua(args.get<RunLuaRequest>(), cancellation);
  };
}

void ToolExecutor::SetSessionId(const std::string& session_id) {
  if (session_id == session_id_ && !session_id_.empty()) {
    // If we're in the same session, we should still refresh skills to pick up
    // changes made by the user via slash commands or direct DB updates.
    auto skills_or = db_->GetActiveSkills(session_id_);
    if (skills_or.ok()) {
      active_skills_.clear();
      for (const auto& skill : *skills_or) {
        active_skills_.insert(skill);
      }
    }
    return;
  }
  session_id_ = session_id;
  active_skills_.clear();
  auto skills_or = db_->GetActiveSkills(session_id_);
  if (skills_or.ok()) {
    for (const auto& skill : *skills_or) {
      active_skills_.insert(skill);
    }
  } else {
    LOG(WARNING) << "Failed to load active skills for session " << session_id << ": " << skills_or.status().message();
  }
}

bool ToolExecutor::IsSkillActive(const std::string& name) const { return active_skills_.contains(name); }

std::vector<std::string> ToolExecutor::GetActiveSkills() const {
  return {active_skills_.begin(), active_skills_.end()};
}



absl::StatusOr<std::string> ToolExecutor::Execute(const std::string& name, const nlohmann::json& args,
                                                  std::shared_ptr<CancellationRequest> cancellation) {
  LOG(INFO) << "Executing tool: " << name
            << " with args: " << args.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);

  bool restricted = false;
  if (mail_mode_) {
    restricted = IsMailModelWorkflowTool(name) || IsBaseModificationTool(name);
  } else {
    // Standard mode: Only Mail Model workflow tools are restricted.
    restricted = IsMailModelWorkflowTool(name);
  }

  if (restricted) {
    auto branch_status = CheckStagingBranch();
    if (!branch_status.ok()) {
      return branch_status.status();
    }
  }

  auto wrap_result = [&](const std::string& tool_name, const std::string& content) {
    return absl::StrCat("### TOOL_RESULT: ", tool_name, "\n", content, "\n\n---");
  };

  auto it = dispatch_map_.find(name);
  if (it == dispatch_map_.end()) {
    return absl::NotFoundError("Tool not found: " + name);
  }

  absl::StatusOr<std::string> result = it->second(args, cancellation);

  if (!result.ok()) {
    std::string error_msg = result.status().ToString();
    // Truncate long error messages for logging
    std::string log_msg = error_msg;
    if (size_t first_nl = log_msg.find('\n'); first_nl != std::string::npos) {
      log_msg = log_msg.substr(0, first_nl) + " (multi-line)...";
    }
    if (log_msg.length() > 100) {
      log_msg = log_msg.substr(0, 97) + "...";
    }
    LOG(WARNING) << "Tool " << name << " failed: " << log_msg;
    return wrap_result(name, "Error: " + error_msg);
  }
  LOG(INFO) << "Tool " << name << " succeeded (" << result->size() << " bytes).";
  (void)db_->IncrementToolCallCount(name);
  return wrap_result(name, *result);
}

absl::StatusOr<std::string> ToolExecutor::ReadFile(const ReadFileRequest& req) {
  if (req.start_line && req.end_line && *req.start_line > *req.end_line) {
    return absl::InvalidArgumentError("start_line must be less than or equal to end_line");
  }

  std::ifstream file(req.path);
  if (!file.is_open()) return absl::NotFoundError("Could not open file: " + req.path);

  std::stringstream ss;
  std::string line;
  int current_line = 1;
  int total_lines = 0;
  {
    std::string dummy;
    while (std::getline(file, dummy)) total_lines++;
    file.clear();  // Clear EOF bit
    file.seekg(0, std::ios::beg);
  }

  while (std::getline(file, line)) {
    if ((!req.start_line || current_line >= *req.start_line) && (!req.end_line || current_line <= *req.end_line)) {
      if (req.add_line_numbers) {
        ss << current_line << ": " << line << "\n";
      } else {
        ss << line << "\n";
      }
    }
    current_line++;
    if (req.end_line && current_line > *req.end_line) {
      break;
    }
  }
  std::string result = ss.str();

  int s = req.start_line.value_or(1);
  int e = req.end_line.value_or(total_lines);
  std::string header = absl::Substitute("### FILE: $0 | TOTAL_LINES: $1 | RANGE: $2-$3\n", req.path, total_lines, s, e);

  if (e < total_lines) {
    absl::StrAppend(&result, "\n... [Truncated. Use 'read_file' with start_line=", e + 1, " to see more] ...");
  }

  return header + result;
}

absl::StatusOr<std::string> ToolExecutor::WriteFile(const WriteFileRequest& req) {
  std::ofstream file(req.path);
  if (!file.is_open()) return absl::InternalError("Could not open file for writing: " + req.path);
  file << req.content;
  file.close();

  // Get the size of the content written
  size_t bytes_written = req.content.size();

  // Create a preview of the content (first 3 lines or less)
  std::stringstream preview;
  std::stringstream content_stream(req.content);
  std::string line;
  int line_count = 0;
  while (std::getline(content_stream, line) && line_count < 3) {
    preview << line << "\n";
    line_count++;
  }

  // Return a more detailed result
  std::string result = "File written successfully:\n";
  result += "Path: " + req.path + "\n";
  result += "Bytes written: " + std::to_string(bytes_written) + "\n";
  result += "Preview:\n" + preview.str();

  return result;
}

absl::StatusOr<std::string> ToolExecutor::ApplyPatch(const ApplyPatchRequest& req) {
  std::ifstream ifs(req.path, std::ios::in | std::ios::binary | std::ios::ate);
  if (!ifs.is_open()) return absl::NotFoundError("Could not open file: " + req.path);
  std::ifstream::pos_type fileSize = ifs.tellg();
  ifs.seekg(0, std::ios::beg);
  std::string content(static_cast<size_t>(fileSize), '\0');
  ifs.read(content.data(), fileSize);

  for (const auto& patch : req.patches) {
    if (patch.find.empty()) return absl::InvalidArgumentError("Patch 'find' string cannot be empty");

    size_t pos = content.find(patch.find);
    if (pos == std::string::npos) {
      return absl::NotFoundError(absl::StrCat("Could not find exact match for: ", patch.find));
    }
    if (content.find(patch.find, pos + 1) != std::string::npos) {
      return absl::FailedPreconditionError(absl::StrCat("Ambiguous match for: ", patch.find));
    }

    content.replace(pos, patch.find.length(), patch.replace);
  }

  return WriteFile({req.path, content});
}

absl::StatusOr<std::string> ToolExecutor::QueryDb(const QueryDbRequest& req) { return db_->Query(req.sql); }

absl::StatusOr<std::string> ToolExecutor::ExecuteBash(const ExecuteBashRequest& req,
                                                      std::shared_ptr<CancellationRequest> cancellation) {
  auto res = RunCommand(req.command, cancellation, req.input);
  if (!res.ok()) return res.status();
  std::string output = res->stdout_out;
  if (!res->stderr_out.empty()) {
    if (!output.empty() && output.back() != '\n') output += "\n";
    output += "### STDERR\n" + res->stderr_out;
  }
  if (res->exit_code != 0) {
    return absl::InternalError(absl::StrCat("Command failed with status ", res->exit_code, ": ", output));
  }
  return output;
}



absl::StatusOr<std::string> ToolExecutor::ListDirectory(const ListDirectoryRequest& req,
                                                        std::shared_ptr<CancellationRequest> cancellation) {
  int max_depth = req.depth.value_or(1);

  ExecuteBashRequest git_check_req;
  git_check_req.command = "git rev-parse --is-inside-work-tree";
  auto git_repo_check = ExecuteBash(git_check_req, cancellation);
  if (req.git_only && git_repo_check.ok() && git_repo_check->find("true") != std::string::npos) {
    std::string cmd = "git ls-files --cached --others --exclude-standard";
    if (req.path != ".") {
      cmd += " " + req.path;
    }
    ExecuteBashRequest git_ls_req;
    git_ls_req.command = cmd;
    auto git_res = ExecuteBash(git_ls_req, cancellation);
    if (git_res.ok()) {
      return git_res;
    }
  }

  // Fallback to std::filesystem
  std::stringstream ss;
  if (!std::filesystem::exists(req.path)) return absl::NotFoundError("Directory not found: " + req.path);

  for (const auto& entry : std::filesystem::recursive_directory_iterator(req.path)) {
    auto relative = std::filesystem::relative(entry.path(), req.path);
    int depth = std::distance(relative.begin(), relative.end());
    if (depth > max_depth) continue;

    if (entry.is_directory()) {
      ss << "Directory: " << relative.string() << "/\n";
    } else {
      ss << "File: " << relative.string() << "\n";
    }
  }

  return ss.str();
}



absl::StatusOr<std::string> ToolExecutor::DescribeDb() {
  return db_->Query("SELECT name, sql FROM sqlite_master WHERE type='table'");
}

absl::StatusOr<std::string> ToolExecutor::UseSkill(const UseSkillRequest& req) {
  if (session_id_.empty()) return absl::FailedPreconditionError("No active session");

  if (req.action == "activate") {
    // Increment count
    auto status = db_->IncrementSkillActivationCount(req.name);
    if (!status.ok()) return status;

    // Add to active if not present
    if (!active_skills_.contains(req.name)) {
      active_skills_.insert(req.name);
      // Convert set to vector for DB update
      std::vector<std::string> skills_vec(active_skills_.begin(), active_skills_.end());
      status = db_->SetActiveSkills(session_id_, skills_vec);
      if (!status.ok()) {
        active_skills_.erase(req.name);  // Rollback cache
        return status;
      }
    }

    // Return patch
    auto skills_or = db_->GetSkills();
    if (!skills_or.ok()) return skills_or.status();
    for (const auto& s : *skills_or) {
      if (s.name == req.name) {
        return "Skill '" + req.name + "' activated.\n\n" + s.system_prompt_patch;
      }
    }
    return absl::NotFoundError("Skill not found: " + req.name);
  }

  if (req.action == "deactivate") {
    if (active_skills_.contains(req.name)) {
      active_skills_.erase(req.name);
      std::vector<std::string> skills_vec(active_skills_.begin(), active_skills_.end());
      auto status = db_->SetActiveSkills(session_id_, skills_vec);
      if (!status.ok()) {
        active_skills_.insert(req.name);  // Rollback cache
        return status;
      }
      return "Skill '" + req.name + "' deactivated.";
    }
    return "Skill '" + req.name + "' was not active.";
  }

  return absl::InvalidArgumentError("Unknown action: " + req.action);
}

absl::StatusOr<std::string> ToolExecutor::RunLua(const RunLuaRequest& req,
                                                 std::shared_ptr<CancellationRequest> cancellation,
                                                 bool raw) {
  slop::Interpreter interpreter;
  sol::state& lua = interpreter.state();

  // Redirect print
  std::stringstream stdout_buffer;
  lua.set_function("print", [&stdout_buffer, &lua](sol::variadic_args args) {
    sol::function tostring = lua["tostring"];
    for (auto arg : args) {
      std::string s = tostring(arg);
      stdout_buffer << s << "\t";
    }
    stdout_buffer << "\n";
  });

  // Inject context
  if (db_) {
    auto settings_or = db_->GetContextSettings(session_id_);
    int window_size = settings_or.ok() ? settings_or->size : 0;
    
    auto history_or = db_->GetConversationHistory(session_id_, false, window_size);
    if (history_or.ok()) {
      sol::table history_table = lua.create_table();
      for (size_t i = 0; i < history_or->size(); ++i) {
        const auto& msg = (*history_or)[i];
        sol::table msg_table = lua.create_table();
        msg_table["role"] = msg.role;
        msg_table["content"] = msg.content;
        msg_table["created_at"] = msg.created_at;
        msg_table["status"] = msg.status;
        msg_table["tool_call_id"] = msg.tool_call_id;
        history_table[i + 1] = msg_table;
      }
      lua["history"] = history_table;
    }

    auto state_or = db_->GetSessionState(session_id_);
    lua["state"] = state_or.ok() ? *state_or : "";

    auto scratchpad_or = db_->GetScratchpad(session_id_);
    lua["scratchpad"] = scratchpad_or.ok() ? *scratchpad_or : "";

    lua["session_id"] = session_id_;

    sol::table json_lib = lua.create_named_table("JSON");
    json_lib.set_function("encode", [](sol::object obj) { return LuaToJSON(obj).dump(); });
    json_lib.set_function("decode", [&lua](const std::string& str) {
      return JSONToLua(lua, nlohmann::json::parse(str));
    });
  }

  // Create 'tools' table
  sol::table tools = lua.create_named_table("tools");

  for (auto const& entry : dispatch_map_) {
    const std::string& name = entry.first;
    // Avoid infinite recursion
    if (name == "run_lua") continue;

    // Skip tools that are implemented in Lua to avoid recursion and allow Lua definitions to take precedence.
    if (lua_tools_.contains(name)) continue;

    tools.set_function(name, [this, name, cancellation](sol::table args_table, sol::this_state s) {
      nlohmann::json json_args = LuaToJSON(args_table);
      
      auto it = dispatch_map_.find(name);
      if (it == dispatch_map_.end()) {
        luaL_error(s, "Tool not found: %s", name.c_str());
        return std::string(""); // Unreachable
      }

      auto result = it->second(json_args, cancellation);
      
      if (!result.ok()) {
        luaL_error(s, "Error: %s", std::string{result.status().message()}.c_str());
        return std::string(""); // Unreachable
      }
      return *result;
    });
  }

  // Execute preamble
  auto preamble_result = lua.safe_script(slop::kLuaPreamble, sol::script_pass_on_error);
  if (!preamble_result.valid()) {
    sol::error err = preamble_result;
    return absl::InternalError(absl::StrCat("Lua Preamble Error: ", err.what()));
  }

  // Execute the script
  if (req.args.is_object() || req.args.is_array()) {
    lua["args"] = JSONToLua(lua, req.args);
  }

  auto result = lua.safe_script(req.script, sol::script_pass_on_error);

  if (!result.valid()) {
    sol::error err = result;
    std::string msg = err.what();
    if (absl::StrContains(msg, "FAILED_PRECONDITION:")) {
      return absl::FailedPreconditionError("No active session");
    }
    return absl::InternalError(absl::StrCat("Lua Error: ", msg, "\nOutput:\n", stdout_buffer.str()));
  }

  // Combine output and return value if any
  if (raw) {
    if (result.return_count() > 0) {
      sol::object rv = result[0];
      sol::function tostring = lua["tostring"];
      return tostring(rv).get<std::string>();
    }
    return stdout_buffer.str();
  }

  std::string output = stdout_buffer.str();
  if (result.return_count() > 0) {
    sol::object rv = result[0];
    sol::function tostring = lua["tostring"];
    std::string s = tostring(rv);
    output += "\nReturn Value: " + s;
  }

  return output;
}

absl::StatusOr<std::string> ToolExecutor::RunLuaTool(
    const std::string& name, const nlohmann::json& args,
    std::shared_ptr<CancellationRequest> cancellation) {
  RunLuaRequest req;
  req.script = "return tools." + name + "(args)";
  req.args = args;
  return RunLua(req, cancellation, /*raw=*/true);
}

bool ToolExecutor::IsMailModelWorkflowTool(const std::string& name) {
  static const std::unordered_set<std::string> tools = {"git_commit_patch", "git_reroll_patch", "git_verify_series",
                                                        "git_format_patch_series", "git_finalize_series"};
  return tools.count(name) > 0;
}

bool ToolExecutor::IsBaseModificationTool(const std::string& name) {
  static const std::unordered_set<std::string> tools = {"write_file", "apply_patch", "execute_bash"};
  return tools.count(name) > 0;
}

// Environment Variable Overrides for Mail Model Enforcement:
//
// SLOP_FORCE_BRANCH_NAME:
//   Used to simulate being on a specific branch. If set and non-empty,
//   GetCurrentBranch() returns this value instead of querying git.
//   Useful for testing enforcement logic without changing the repo state.
//
// SLOP_SKIP_STAGING_CHECK:
//   If set to "1", CheckStagingBranch() will always succeed with "skipped".
//   Used in unit tests to allow setup/teardown with protected tools (like write_file)
//   without needing a staging branch.

std::optional<std::string> ToolExecutor::GetForcedBranch() {
  const char* forced_branch = std::getenv("SLOP_FORCE_BRANCH_NAME");
  if (forced_branch && strlen(forced_branch) > 0) {
    return std::string(forced_branch);
  }
  return std::nullopt;
}

bool ToolExecutor::ShouldSkipStagingCheck() {
  const char* skip_check = std::getenv("SLOP_SKIP_STAGING_CHECK");
  return skip_check && std::string(skip_check) == "1";
}

absl::StatusOr<std::string> ToolExecutor::GetCurrentBranch() {
  if (auto forced = GetForcedBranch()) {
    return *forced;
  }

  auto branch_res = RunCommand("git rev-parse --abbrev-ref HEAD");
  if (!branch_res.ok()) return branch_res.status();
  if (branch_res->exit_code != 0) {
    return absl::InternalError("Failed to get current branch: " + branch_res->stderr_out);
  }
  std::string current_branch = branch_res->stdout_out;
  if (!current_branch.empty() && current_branch.back() == '\n') current_branch.pop_back();
  return current_branch;
}

absl::StatusOr<std::string> ToolExecutor::CheckStagingBranch() {
  if (ShouldSkipStagingCheck()) {
    return std::string("skipped");
  }

  auto branch_res = GetCurrentBranch();
  if (!branch_res.ok()) {
    // If getting the branch fails, we assume we might not be in a git repo.
    // We allow this to support environments without git (like some bazel sandboxes).
    return std::string("not-a-git-repo");
  }

  std::string current_branch = *branch_res;
  if (!absl::StartsWith(current_branch, "slop/staging/")) {
    return absl::FailedPreconditionError(
        "Mail Model Violation: This tool is restricted to staging branches (slop/staging/*). "
        "You are currently on '" +
        current_branch + "'. Please use git_branch_staging to start a new series.");
  }
  return current_branch;
}

absl::StatusOr<std::string> ToolExecutor::GetBaseBranch(const std::string& requested_base) {
  // Primarily used for testing (mail_model_test.cpp).
  // Wraps the Lua implementation to maintain a single source of truth.
  RunLuaRequest req;
  req.script = "return git.get_base_branch(args.requested_base)";
  req.args["requested_base"] = requested_base;
  return RunLua(req, nullptr, /*raw=*/true);
}

} // namespace slop
