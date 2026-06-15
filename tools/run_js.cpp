
#include <string>

#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

#include "core/json_utils.h"
#include "js_bridge/interpreter.h"

namespace slop {

absl::StatusOr<std::string> HandleRunJsTool(const nlohmann::json& args) {
  absl::StatusOr<nlohmann::json> result_or = ExecuteRunJsArgs(args);
  if (!result_or.ok()) {
    return result_or.status();
  }
  return json_dump(*result_or);
}

}  // namespace slop