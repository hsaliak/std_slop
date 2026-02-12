#pragma once

#include <string>
#include <vector>
#include <memory>
#include <sol/sol.hpp>

namespace slop {

class Interpreter {
public:
    Interpreter();
    ~Interpreter() = default;

    // Load standard libraries. By default, all are loaded in the constructor.
    void LoadStandardLibs();

    // Run a lua script from a string.
    sol::protected_function_result RunString(const std::string& code);

    // Run a lua script from a file.
    sol::protected_function_result RunFile(const std::string& path);

    // Access the underlying sol::state
    sol::state& state() { return *lua_; }

private:
    std::unique_ptr<sol::state> lua_;
};

} // namespace slop
