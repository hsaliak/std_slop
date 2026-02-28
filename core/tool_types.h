#ifndef SLOP_SQL_TOOL_TYPES_H_
#define SLOP_SQL_TOOL_TYPES_H_

#include <string>

#include "nlohmann/json.hpp"

#include "json_utils.h"

namespace slop {

struct RunJsRequest {
  std::string script;
  nlohmann::json args;
};

inline void from_json(const nlohmann::json& j, RunJsRequest& r) {
  r.script = json_get_or(j, "script", std::string{});
  r.args = json_get_or(j, "args", nlohmann::json{});
}

}  // namespace slop

#endif  // SLOP_SQL_TOOL_TYPES_H_
