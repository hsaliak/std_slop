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
  dispatch_map_["apply_patch"] = [this](const nlohmann::json& args,
                                         std::shared_ptr<CancellationRequest>) {
    return ApplyPatch(args.get<ApplyPatchRequest>());
  };
  dispatch_map_["query_db"] = [this](const nlohmann::json& args,
                                     std::shared_ptr<CancellationRequest>) -> absl::StatusOr<std::string> {
    if (!db_) return absl::InternalError("No database");
    std::vector<std::string> params;
    if (args.contains("params")) {
      for (const auto& p : args["params"]) {
        if (p.is_string()) {
          params.push_back(p.get<std::string>());
        } else {
          params.push_back(p.dump());
        }
      }
    }
    return db_->Query(args.at("sql").get<std::string>(), params);
  };
  dispatch_map_["run_lua"] = [this](const nlohmann::json& args,
                                    std::shared_ptr<CancellationRequest> cancellation) {
    return RunLua(args.get<RunLuaRequest>(), cancellation);
  };

}

absl::StatusOr<std::string> ToolExecutor::Execute(const std::string& name, const nlohmann::json& args,
                                                  std::shared_ptr<CancellationRequest> cancellation) {
  if (name == "run_lua") {
    RunLuaRequest req;
    req.script = args.at("script").get<std::string>();
    if (args.contains("args")) req.args = args["args"];
    return RunLua(req, cancellation);
  }

  // Use Lua orchestrator for all other tools
  RunLuaRequest req;
  req.script = "return core.dispatch_tool(args.name, args.tool_args)";
  req.args["name"] = name;
  req.args["tool_args"] = nlohmann::json(args);

  auto res = RunLua(req, cancellation, /*raw=*/true);
  if (!res.ok()) {
    std::string msg = std::string(res.status().message());
    if (absl::StrContains(msg, "NOT_FOUND:")) {
      return absl::NotFoundError(msg);
    }
    if (absl::StrContains(msg, "FAILED_PRECONDITION:")) {
      return absl::FailedPreconditionError(msg);
    }
    if (absl::StrContains(msg, "INVALID_ARGUMENT:")) {
      return absl::InvalidArgumentError(msg);
    }
    return res;
  }

  if (db_) {
    (void)db_->IncrementToolCallCount(name);
  }
  return res;
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

  sol::table json_lib = lua.create_table();
  json_lib["parse"] = [](const std::string& s, sol::this_state st) {
    sol::state_view lua(st);
    auto j = nlohmann::json::parse(s, nullptr, false);
    if (j.is_discarded()) {
      return sol::make_object(lua, sol::lua_nil);
    }
    return JSONToLua(lua, j);
  };
  json_lib["stringify"] = [](sol::object obj) {
    return LuaToJSON(obj).dump();
  };
  lua["JSON"] = json_lib;

  lua["session_id"] = session_id_;
  if (!req.args.is_null()) {
    lua["args"] = JSONToLua(lua, req.args);
  }

  sol::table tools = lua.create_named_table("tools");

  for (auto const& pair : dispatch_map_) {
    const std::string& name = pair.first;
    if (name == "run_lua") continue;
    tools.set_function(name, [this, name, cancellation](sol::table args_table, sol::this_state s) {
      LOG(INFO) << "[LUA->C++] Call: " << name;
      nlohmann::json json_args = LuaToJSON(args_table);
      auto it = dispatch_map_.find(name);
      auto result = it->second(json_args, cancellation);

      if (!result.ok()) {
        LOG(INFO) << "[LUA->C++] " << name << " FAILED: " << result.status().ToString();
        std::string err_msg = absl::StrCat(absl::StatusCodeToString(result.status().code()), ": ", result.status().message());
        luaL_error(s, "Error: %s", err_msg.c_str());
        return std::string("");
      }
      LOG(INFO) << "[LUA->C++] " << name << " SUCCESS";
      return *result;
    });
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


  if (!result.valid()) {
    sol::error err = result;
    return absl::InternalError(absl::StrCat("Lua Error: ", err.what(), "\nOutput:\n", stdout_buffer.str()));
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




absl::StatusOr<std::string> ToolExecutor::GetBaseBranch(const std::string& requested_base) {
  RunLuaRequest req;
  req.script = "return git.get_base_branch(args.requested_base)";
  req.args["requested_base"] = requested_base;
  return RunLua(req, nullptr, /*raw=*/true);
}

}  // namespace slop
