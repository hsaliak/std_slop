#include "core/tool_executor.h"

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "core/status_macros.h"
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
      auto parsed = json_parse(s);
      if (!parsed.has_value()) {
        return absl::InvalidArgumentError(absl::StrCat("Failed to parse ", context, ": invalid JSON"));
      }
      if (parsed->is_array()) return *parsed;
      return absl::InvalidArgumentError(absl::StrCat("Unexpected result shape for ", context));
    }
    if (v.is_object()) {
      if (auto rows = json_get<nlohmann::json::array_t>(v, "rows")) {
        return nlohmann::json(*rows);
      }
    }
    return absl::InvalidArgumentError(absl::StrCat("Unexpected result shape for ", context));
  };

  ASSIGN_OR_RETURN(auto rows, parse_for_context(value, json_get_or<std::string>(args, "context", "context")));
  return rows.dump();
}

}  // namespace slop
