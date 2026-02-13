#include "core/tool_executor.h"
#include "lua-bridge/interpreter.h"
#include "core/lua_bridge_util.h"
#include "core/preamble_data.h"

#include <algorithm>
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
  dispatch_map_["execute_bash"] = [this](const nlohmann::json& args, auto cancellation) {
    return ExecuteBash(args.get<ExecuteBashRequest>(), cancellation);
  };
  dispatch_map_["query_db"] = [this](const nlohmann::json& args, auto) {
    return QueryDb(args.get<QueryDbRequest>());
  };
  dispatch_map_["run_lua"] = [this](const nlohmann::json& args, auto cancellation) {
    return RunLua(args.get<RunLuaRequest>(), cancellation);
  };

  // Lua-implemented tools
  lua_tools_ = {
      "list_directory",
      "grep_tool",
      "git_grep_tool",
      "search_code",
      "save_memo",
      "retrieve_memos",
      "manage_scratchpad",
      "describe_db",
      "use_skill",
      "git_branch_staging",
      "git_commit_patch",
      "git_reroll_patch",
      "git_verify_series",
      "git_format_patch_series",
      "git_finalize_series",
  };

  for (const auto& name : lua_tools_) {
    dispatch_map_[name] =
        [this, name](const nlohmann::json& args,
                     std::shared_ptr<CancellationRequest> cancellation) {
          return RunLuaTool(name, args, cancellation);
        };
  }
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

  auto it = dispatch_map_.find(name);
  if (it == dispatch_map_.end()) {
    return absl::NotFoundError("Tool not found: " + name);
  }

  absl::StatusOr<std::string> result = it->second(args, cancellation);

  if (!result.ok()) {
    std::string error_msg = absl::StrCat(absl::StatusCodeToString(result.status().code()), ": ", result.status().message());
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
  if (db_) (void)db_->IncrementToolCallCount(name);
  return wrap_result(name, *result);
}

std::string ToolExecutor::wrap_result(const std::string& name, const std::string& result) {
  return "### TOOL_RESULT: " + name + "\n" + result + "\n---";
}

void ToolExecutor::SetSessionId(const std::string& session_id) {
  session_id_ = session_id;
}

bool ToolExecutor::IsSkillActive(const std::string& name) {
  auto active = GetActiveSkills();
  return std::any_of(active.begin(), active.end(),
                     [&name](const std::string& s) { return s == name; });
}

std::vector<std::string> ToolExecutor::GetActiveSkills() {
  if (session_id_.empty() || !db_) return {};
  auto skills_or = db_->GetActiveSkills(session_id_);
  if (skills_or.ok()) {
    return *skills_or;
  }
  return {};
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
    file.clear();
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

  size_t bytes_written = req.content.size();
  std::stringstream preview;
  std::stringstream content_stream(req.content);
  std::string line;
  int line_count = 0;
  while (std::getline(content_stream, line) && line_count < 3) {
    preview << line << "\n";
    line_count++;
  }

  std::string result = "File written successfully:\n";
  result += "Path: " + req.path + "\n";
  result += "Bytes written: " + std::to_string(bytes_written) + "\n";
  result += "Preview:\n" + preview.str();
  if (line_count >= 3) result += "...\n";

  return result;
}

absl::StatusOr<std::string> ToolExecutor::ApplyPatch(const ApplyPatchRequest& req) {
  std::string content;
  {
    std::ifstream file(req.path);
    if (!file.is_open()) return absl::NotFoundError("Could not open file: " + req.path);
    content = std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  }

  for (const auto& patch : req.patches) {
    size_t pos = content.find(patch.find);
    if (pos == std::string::npos) {
      return absl::NotFoundError(absl::StrCat("Could not find exact match for: ", patch.find));
    }
    if (content.find(patch.find, pos + 1) != std::string::npos) {
      return absl::FailedPreconditionError(absl::StrCat("Multiple matches found for: ", patch.find, ". Please use a more specific 'find' block."));
    }
    content.replace(pos, patch.find.length(), patch.replace);
  }

  {
    std::ofstream file(req.path);
    if (!file.is_open()) return absl::InternalError("Could not open file for writing: " + req.path);
    file << content;
  }
  return "File written successfully: " + req.path;
}

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

absl::StatusOr<std::string> ToolExecutor::QueryDb(const QueryDbRequest& req) {
  return db_->Query(req.sql);
}

absl::StatusOr<std::string> ToolExecutor::RunLua(const RunLuaRequest& req,
                                                 std::shared_ptr<CancellationRequest> cancellation,
                                                 bool raw) {
  slop::Interpreter interpreter;
  sol::state& lua = interpreter.state();

  std::stringstream stdout_buffer;
  lua.set_function("print", [&stdout_buffer, &lua](sol::variadic_args args) {
    sol::function tostring = lua["tostring"];
    std::stringstream ss;
    for (auto arg : args) {
      std::string s = tostring(arg);
      ss << s << "\t";
    }
    LOG(INFO) << "[LUA] " << ss.str();
    stdout_buffer << ss.str() << "\n";
  });

  lua.set_function("__os_run", [cancellation](const std::string& command, sol::this_state s) -> sol::table {
    auto res_or = RunCommand(command, cancellation);
    sol::state_view lua(s);
    sol::table t = lua.create_table();
    if (!res_or.ok()) {
      t["stdout"] = "";
      t["stderr"] = res_or.status().ToString();
      t["exit_code"] = -1;
      return t;
    }
    t["stdout"] = res_or->stdout_out;
    t["stderr"] = res_or->stderr_out;
    t["exit_code"] = res_or->exit_code;
    return t;
  });

  lua["session_id"] = session_id_;
  lua["scratchpad"] = "";
  lua["state"] = "";

  if (!session_id_.empty() && db_) {
    auto scratchpad_or = db_->GetScratchpad(session_id_);
    if (scratchpad_or.ok()) lua["scratchpad"] = *scratchpad_or;

    auto state_or = db_->GetSessionState(session_id_);
    if (state_or.ok()) lua["state"] = *state_or;

    auto settings_or = db_->GetContextSettings(session_id_);
    int window_size = settings_or.ok() ? settings_or->size : 0;

    auto history_or = db_->GetConversationHistory(session_id_, false, window_size);
    if (history_or.ok()) {
      sol::table history = lua.create_table();
      for (size_t i = 0; i < history_or->size(); ++i) {
        const auto& msg = (*history_or)[i];
        sol::table msg_table = lua.create_table();
        msg_table["role"] = msg.role;
        msg_table["content"] = msg.content;
        history[i + 1] = msg_table;
      }
      lua["history"] = history;
    }
  }

  {
    sol::table json_lib = lua.create_named_table("JSON");
    json_lib.set_function("encode", [](sol::object obj) { return LuaToJSON(obj).dump(); });
    json_lib.set_function("decode", [&lua](const std::string& str) {
      return JSONToLua(lua, nlohmann::json::parse(str));
    });
    json_lib.set_function("stringify", [](sol::object obj) { return LuaToJSON(obj).dump(); });
    json_lib.set_function("parse", [&lua](const std::string& str) {
      return JSONToLua(lua, nlohmann::json::parse(str));
    });
  }

  sol::table tools = lua.create_named_table("tools");

  for (auto const& pair : dispatch_map_) {
    const std::string& name = pair.first;
    if (name == "run_lua") continue;
    if (lua_tools_.contains(name)) continue;
    tools.set_function(name, [this, name, cancellation](sol::table args_table, sol::this_state s) {
      LOG(INFO) << "[LUA->C++] Call: " << name;
      nlohmann::json json_args = LuaToJSON(args_table);
      auto it = dispatch_map_.find(name);
      auto result = it->second(json_args, cancellation);

      if (!result.ok()) {
        LOG(INFO) << "[LUA->C++] " << name << " FAILED: " << result.status().ToString();
        luaL_error(s, "Error: %s", std::string{result.status().message()}.c_str());
        return std::string("");
      }
      LOG(INFO) << "[LUA->C++] " << name << " SUCCESS";
      return *result;
    });
  }

  if (!req.args.is_null()) {
    lua["args"] = JSONToLua(lua, req.args);
  }

  auto lib_result = lua.safe_script(slop::kLuaPreambleLib, sol::script_pass_on_error);
  if (!lib_result.valid()) {
    sol::error err = lib_result;
    return absl::InternalError(absl::StrCat("Lua Preamble Lib Error: ", err.what()));
  }

  auto preamble_result = lua.safe_script(slop::kLuaPreamble, sol::script_pass_on_error);
  if (!preamble_result.valid()) {
    sol::error err = preamble_result;
    return absl::InternalError(absl::StrCat("Lua Preamble Error: ", err.what()));
  }

  auto result = lua.safe_script(req.script, sol::script_pass_on_error);

  if (!session_id_.empty() && db_) {
    std::string final_scratchpad = lua["scratchpad"];
    auto old_scratchpad = db_->GetScratchpad(session_id_);
    if (final_scratchpad != (old_scratchpad.ok() ? *old_scratchpad : "")) {
      (void)db_->UpdateScratchpad(session_id_, final_scratchpad);
    }
    std::string final_state = lua["state"];
    auto old_state = db_->GetSessionState(session_id_);
    if (final_state != (old_state.ok() ? *old_state : "")) {
      (void)db_->SetSessionState(session_id_, final_state);
    }
  }

  if (!result.valid()) {
    sol::error err = result;
    std::string msg = err.what();
    if (absl::StrContains(msg, "FAILED_PRECONDITION:")) {
      return absl::FailedPreconditionError("No active session");
    }
    return absl::InternalError(absl::StrCat("Lua Error: ", msg, "\nOutput:\n", stdout_buffer.str()));
  }

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

absl::StatusOr<std::string> ToolExecutor::RunLuaTool(const std::string& name, const nlohmann::json& args,
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
  RunLuaRequest req;
  req.script = "return git.get_base_branch(args.requested_base)";
  req.args["requested_base"] = requested_base;
  return RunLua(req, nullptr, /*raw=*/true);
}

}  // namespace slop
