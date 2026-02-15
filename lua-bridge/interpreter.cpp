#include "lua-bridge/interpreter.h"

namespace slop {

Interpreter::Interpreter() : lua_(std::make_unique<sol::state>()) { LoadStandardLibs(); }

void Interpreter::LoadStandardLibs() {
  lua_->open_libraries(sol::lib::base, sol::lib::package, sol::lib::coroutine, sol::lib::string, sol::lib::os,
                       sol::lib::math, sol::lib::table, sol::lib::debug, sol::lib::bit32, sol::lib::io );
}

sol::protected_function_result Interpreter::RunString(const std::string& code) { return lua_->safe_script(code); }

sol::protected_function_result Interpreter::RunFile(const std::string& path) { return lua_->safe_script_file(path); }

}  // namespace slop
