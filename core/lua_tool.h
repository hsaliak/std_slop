#ifndef SLOP_CORE_LUA_TOOL_H_
#define SLOP_CORE_LUA_TOOL_H_

#include <functional>
#include <memory>
#include <sstream>
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"
#include "sol/sol.hpp"

namespace slop {
class Database;
class CancellationRequest;
}  // namespace slop

namespace slop::lua_tool {

// Maps tool names to their C++ implementations.
using ToolDispatchMap =
    absl::flat_hash_map<std::string,
                        std::function<absl::StatusOr<std::string>(
                            const nlohmann::json&,
                            std::shared_ptr<CancellationRequest>)>>;

// Initializes a Lua state with the standard slop environment:
// - print: redirects to stdout_buffer
// - JSON.parse / JSON.stringify
// - __os_run: shell execution
// - tools: table containing C++ tool wrappers from the dispatch map
void InitializeEnvironment(
    sol::state& lua, Database* db,
    std::shared_ptr<CancellationRequest> cancellation,
    const ToolDispatchMap& dispatch_map, std::stringstream& stdout_buffer);

}  // namespace slop::lua_tool

#endif  // SLOP_CORE_LUA_TOOL_H_
