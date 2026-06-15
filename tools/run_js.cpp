
#include <functional>
#include <string>
#include <utility>

#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

#include "core/json_utils.h"
#include "js_bridge/interpreter.h"

namespace slop {

absl::StatusOr<std::string> HandleRunJsTool(
    const nlohmann::json& args,
    std::function<absl::StatusOr<std::string>(const std::string&, const nlohmann::json&)> tool_caller) {
  absl::StatusOr<nlohmann::json> result_or = ExecuteRunJsArgs(args, std::move(tool_caller));
  if (!result_or.ok()) {
    return result_or.status();
  }
  return json_dump(*result_or);
}

}  // namespace slop