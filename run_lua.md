# Plan: Implementing the `run_lua` Skill

## Core Objective
Implement a `run_lua` skill that allows the LLM to execute Lua 5.4 scripts. This skill will act as a "Meta-Tool," providing a scripting environment where existing C++ tools are available as Lua functions and the full Lua standard library is accessible.

## Architecture: The Automated Logic Bridge
To ensure every tool is available without manual boilerplate, we will implement a **Recursive Translation Bridge** between `sol2` (Lua) and `nlohmann::json` (C++).

*   **Generic Converter**: We will build two helper functions:
    *   `LuaToJSON`: Recursively converts Lua tables, strings, numbers, and booleans into a `nlohmann::json` object.
    *   `JSONToLua`: Converts tool results and JSON objects back into Lua tables or strings.
*   **Dynamic Binding**: The `ToolExecutor` will iterate over its `dispatch_map_` (which contains all registered tools). For every entry, it will create a corresponding function in a global Lua table named `tools`.
    *   **Calling Convention**: `success, result = tools.tool_name({arg1 = "val", arg2 = 123})`

## Environment Configuration
*   **Standard Library**: The environment will be initialized with `luaL_openlibs`, granting unrestricted access to `io`, `os`, `math`, `string`, `table`, etc.
*   **Output Capture**: The global `print` function will be redirected to a C++ `std::stringstream`. This buffer will be appended to the final result returned to the LLM.
*   **Fresh States**: By default, each `run_lua` call will instantiate a clean `slop::Interpreter` to ensure side-effect isolation and predictability.

## Verification & "Every-Tool" Testing Strategy
1. **Parity Verification (C++)**: Reflectively compare `dispatch_map_` keys against the Lua `tools` table.
2. **Full Spectrum Integration Test (Lua)**: A script that iterates through every tool and verifies it is callable.
3. **Complex Orchestration Test**: A real-world script test (e.g., list files, read one).
4. **Resilience Tests**: Deep tables, stdout integrity, and error propagation.

## Implementation Roadmap
1. **Infrastructure**: Update `core/BUILD.bazel` to link `//lua-bridge:interpreter` and `sol2`.
2. **Bridge Development**: Implement `LuaToJSON` and `JSONToLua` in `core/lua_bridge_util.h`.
3. **Tool Integration**: Implement `ToolExecutor::RunLua` and the dynamic `tools` table binder.
4. **Verification**: Write the `Full Spectrum` test suite in `core/tool_executor_test.cpp`.
5. **Documentation**: Register the `run_lua` skill in the database.
