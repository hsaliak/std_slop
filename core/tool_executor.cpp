#include "core/tool_executor.h"
#include "json_utils.h"

#include <algorithm>
#include <memory>
#include <sstream>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

#include "core/database.h"
#include "core/lua_bridge_util.h"
#include "core/lua_tool.h"
#include "core/preamble_data.h"
#include "core/tool_dispatcher.h"
#include "lua-bridge/interpreter.h"

namespace slop {

ToolExecutor::ToolExecutor(Database* db) : db_(db) { RegisterTools(); }

ToolExecutor::~ToolExecutor() = default;

void ToolExecutor::SetDispatcher(std::unique_ptr<ToolDispatcher> dispatcher) { dispatcher_ = std::move(dispatcher); }

void ToolExecutor::RegisterTool(const std::string& name, ToolHandler handler) {
  dispatch_map_[name] = std::move(handler);
}

absl::StatusOr<std::string> ToolExecutor::HandleQueryDb(const nlohmann::json& args) {
  if (!db_) {
    return absl::FailedPreconditionError("Database not initialized");
  }
  if (!args.is_object()) {
    return absl::InvalidArgumentError("Arguments must be a JSON object");
  }

  auto sql = json_get<std::string>(args, "sql");
  if (!sql) {
    return absl::InvalidArgumentError("'sql' must be a string");
  }

  std::vector<std::string> params;
  if (auto p_array = json_get<nlohmann::json::array_t>(args, "params")) {
    for (const auto& p : *p_array) {
      if (p.is_string()) {
        params.push_back(p.get<std::string>());
      } else if (p.is_null()) {
        params.emplace_back("NULL");
      } else {
        // For numbers, booleans, objects, and arrays, stringify them.
        params.push_back(p.dump());
      }
    }
  }
  return db_->Query(*sql, params);
}


absl::StatusOr<std::string> ToolExecutor::HandleRunLua(const nlohmann::json& args,
                                                       std::shared_ptr<CancellationRequest> cancellation) {
  RunLuaRequest req;
  auto script = json_get<std::string>(args, "script");
  if (!script) {
    return absl::InvalidArgumentError("'script' must be a string");
  }
  req.script = *script;
  if (auto* lua_args = json_at(args, "args")) req.args = *lua_args;
  auto res = RunLua(req, cancellation);
  if (!res.ok()) return res.status();
  return res->FullOutput();
}

}

void ToolExecutor::RegisterTools() {
  RegisterTool("query_db", [this](const nlohmann::json& args, auto) { return HandleQueryDb(args); });

  RegisterTool("run_lua", [this](const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
    return HandleRunLua(args, cancellation);
  });
}

absl::StatusOr<std::string> ToolExecutor::Execute(const std::string& name, const nlohmann::json& args,
                                                  std::shared_ptr<CancellationRequest> cancellation) {
  auto it = dispatch_map_.find(name);
  if (it != dispatch_map_.end()) {
    auto res = it->second(args, cancellation);
    if (res.ok() && db_) {
      (void)db_->IncrementToolCallCount(name);
    }
    return res;
  }

  // Use Lua orchestrator for all other tools.
  RunLuaRequest req;
  req.script = "return core.dispatch_tool(args.name, args.tool_args)";
  req.args["name"] = name;
  req.args["tool_args"] = args;

  auto res = RunLua(req, cancellation);
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
    return res.status();
  }

  if (db_) {
    (void)db_->IncrementToolCallCount(name);
  }
  return res->return_value;
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

absl::StatusOr<ToolExecutor::LuaResult> ToolExecutor::RunLua(const RunLuaRequest& req,
                                                             std::shared_ptr<CancellationRequest> cancellation) {
  slop::Interpreter interpreter;
  sol::state& lua = interpreter.state();

  std::stringstream stdout_buffer;
  lua_tool::InitializeEnvironment(lua, db_, dispatcher_.get(), cancellation, dispatch_map_, stdout_buffer);

  lua["session_id"] = session_id_;

  // Inject scratchpad
  auto scratchpad_res = db_->GetScratchpad(session_id_);
  if (scratchpad_res.ok() && !scratchpad_res->empty()) {
    lua["scratchpad"] = *scratchpad_res;
  }

  // Inject state
  auto state_res = db_->GetSessionState(session_id_);
  if (state_res.ok() && !state_res->empty()) {
    lua["state"] = *state_res;
  }

  // Inject history
  auto history_res = db_->GetConversationHistory(session_id_);
  if (history_res.ok() && !history_res->empty()) {
    sol::table history_table = lua.create_table();
    int i = 1;
    for (const auto& msg : *history_res) {
      sol::table msg_table = lua.create_table();
      msg_table["role"] = msg.role;
      msg_table["content"] = msg.content;
      history_table[i++] = msg_table;
    }
    lua["history"] = history_table;
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
  if (!result.valid()) {
    sol::error err = result;
    return absl::InternalError(absl::StrCat("Lua Error: ", err.what(), "\nOutput:\n", stdout_buffer.str()));
  }

  LuaResult res;
  res.stdout_out = stdout_buffer.str();
  if (result.return_count() > 0) {
    res.return_value = result[0].as<std::string>();
  }

  return res;
}

absl::StatusOr<std::string> ToolExecutor::GetBaseBranch(const std::string& requested_base) {
  RunLuaRequest req;
  req.script = "return git.get_base_branch(args.requested_base)";
  req.args["requested_base"] = requested_base;
  auto res = RunLua(req, nullptr);
  if (!res.ok()) return res.status();
  return res->return_value;
}

}  // namespace slop