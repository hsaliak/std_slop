#include "core/lua_tool.h"

#include "absl/log/log.h"
#include "absl/strings/str_cat.h"

#include "core/database.h"
#include "core/lua_bridge_util.h"
#include "core/shell_util.h"
#include "core/tool_dispatcher.h"

namespace slop::lua_tool {

void InitializeEnvironment(sol::state& lua, [[maybe_unused]] Database* db, ToolDispatcher* dispatcher,
                           std::shared_ptr<CancellationRequest> cancellation, const ToolDispatchMap& dispatch_map,
                           std::stringstream& stdout_buffer) {
  // Custom print function that redirects to stdout_buffer
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

  // OS shell execution
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

  // JSON library
  sol::table json_lib = lua.create_table();
  json_lib["parse"] = [](const std::string& s, sol::this_state st) {
    sol::state_view lua(st);
    auto j = nlohmann::json::parse(s, nullptr, false);
    if (j.is_discarded()) {
      return sol::make_object(lua, sol::lua_nil);
    }
    return JSONToLua(lua, j);
  };
  json_lib["stringify"] = [](sol::object obj) { return SafeDump(LuaToJSON(obj)); };
  lua["JSON"] = json_lib;

  // Register ToolJob class
  lua.new_usertype<ToolJob>("ToolJob", "is_ready", &ToolJob::IsReady, "wait", [](ToolJob& self, sol::this_state s) {
    auto result = self.Wait();
    if (!result.ok()) {
      luaL_error(s, "Job failed: %s", result.status().ToString().c_str());
    }
    return *result;
  });

  // Tool dispatcher
  sol::table tools = lua.create_named_table("tools");

  // tools.dispatch_async
  if (dispatcher) {
    tools.set_function("dispatch_async", [dispatcher, cancellation](const std::string& name, sol::table args_table) {
      ToolDispatcher::Call call;
      call.id = "async-" + name;  // Simple ID
      call.name = name;
      call.args = LuaToJSON(args_table);
      return dispatcher->Submit(call, cancellation);
    });
  }
  for (auto const& pair : dispatch_map) {
    const std::string& name = pair.first;
    const auto& handler = pair.second;
    if (name == "run_lua") continue;
    tools.set_function(name, [name, handler, cancellation](sol::table args_table, sol::this_state s) {
      LOG(INFO) << "[LUA->C++] Call: " << name;
      nlohmann::json json_args = LuaToJSON(args_table);
      auto result = handler(json_args, cancellation);

      if (!result.ok()) {
        LOG(INFO) << "[LUA->C++] " << name << " FAILED: " << result.status().ToString();
        std::string err_msg =
            absl::StrCat(absl::StatusCodeToString(result.status().code()), ": ", result.status().message());
        luaL_error(s, "Error: %s", err_msg.c_str());
        return std::string("");
      }
      LOG(INFO) << "[LUA->C++] " << name << " SUCCESS";
      return *result;
    });
  }
}

}  // namespace slop::lua_tool
