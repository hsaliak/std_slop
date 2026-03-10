#include "core/tool_executor.h"

#include <fstream>
#include <sstream>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"

#include "core/database.h"
#include "core/shell_util.h"
#include "core/status_macros.h"
#include "core/tool_dispatcher.h"
#include "core/tools/common.h"
#include "interface/color.h"
#include "interface/renderer.h"
#include "interface/terminal.h"
#include "core/json_utils.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandleParseToolRows(const nlohmann::json& args) const {
  if (!args.is_object()) {
    return "[]";
  }

  nlohmann::json value = nlohmann::json();
  if (auto it = args.find("value"); it != args.end()) value = *it;

  auto parse_for_context = [](const nlohmann::json& v, const std::string& context)
      -> absl::StatusOr<nlohmann::json> {
    if (v.is_array()) return v;
    if (v.is_null()) return nlohmann::json::array();
    if (v.is_string()) {
      const std::string s = v.get<std::string>();
      if (s.empty()) return nlohmann::json::array();
      auto parsed = nlohmann::json::parse(s, nullptr, false);
      if (parsed.is_discarded()) {
        return absl::InvalidArgumentError(absl::StrCat("Failed to parse ", context, ": invalid JSON"));
      }
      if (parsed.is_array()) return parsed;
      return absl::InvalidArgumentError(absl::StrCat("Unexpected result shape for ", context));
    }
    if (v.is_object() && v.contains("rows") && v["rows"].is_array()) {
      return v["rows"];
    }
    return absl::InvalidArgumentError(absl::StrCat("Unexpected result shape for ", context));
  };

  ASSIGN_OR_RETURN(auto rows, parse_for_context(value, json_get_or<std::string>(args, "context", "context")));
  return rows.dump();
}

}  // namespace slop
