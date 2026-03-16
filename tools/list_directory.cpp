#include <sstream>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"

#include "core/json_utils.h"
#include "core/shell_util.h"
#include "core/status_macros.h"
#include "tools/tool_executor.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandleListDirectory(const nlohmann::json& args) {
  const std::string path = json_get_or<std::string>(args, "path", ".");

  int raw_depth = 1;
  if (auto depth_int = json_get<int>(args, "depth")) {
    raw_depth = *depth_int;
  } else if (auto depth_str = json_get<std::string>(args, "depth")) {
    if (!absl::SimpleAtoi(*depth_str, &raw_depth)) {
      return absl::InvalidArgumentError("depth must be an integer");
    }
  }
  const int depth = std::max(1, std::min(8, raw_depth));

  const bool include_ignored = json_get_or<bool>(args, "include_ignored", false);
  std::vector<std::string> ignore_patterns;
  if (!include_ignored) {
    ignore_patterns = {".git", "node_modules", "bazel-*", "dist", "build", ".cache", ".next", "target"};
    const auto* ignore_json = json_at(args, "ignore");
    if (ignore_json && !ignore_json->is_null()) {
      if (!ignore_json->is_array()) {
        return absl::InvalidArgumentError("ignore must be an array of strings");
      }
      for (const auto& it : *ignore_json) {
        if (it.is_string() && !it.get<std::string>().empty()) {
          ignore_patterns.push_back(it.get<std::string>());
        }
      }
    }
  }

  std::string prune_clause;
  if (!ignore_patterns.empty()) {
    std::vector<std::string> names;
    names.reserve(ignore_patterns.size());
    for (const auto& p : ignore_patterns) {
      names.push_back(absl::StrCat("-name ", EscapeShellArg(p)));
    }
    prune_clause = absl::StrCat(" \\( ", absl::StrJoin(names, " -o "), " \\) -prune -o");
  }

  const std::string cmd = absl::StrCat(
      "cd ", EscapeShellArg(path), " && find . ", " -mindepth 1 -maxdepth ", depth, prune_clause,
      " -mindepth 1 -maxdepth ", depth,
      " -exec sh -c 'for f; do rel=\"${f#./}\"; if [ -d \"$f\" ]; then printf \"d\\t%s\\n\" \"$rel\"; else "
      "printf \"f\\t%s\\n\" \"$rel\"; fi; done' sh {} +");

  ASSIGN_OR_RETURN(auto run_res, RunCommand(cmd));
  if (run_res.exit_code != 0) {
    return absl::InternalError(absl::StrCat("Failed to list directory: ", run_res.stderr_out));
  }

  std::vector<std::string> out;
  std::stringstream ss(run_res.stdout_out);
  std::string row;
  while (std::getline(ss, row)) {
    if (row.empty()) continue;
    size_t tab = row.find('\t');
    if (tab == std::string::npos) continue;
    std::string type = row.substr(0, tab);
    std::string rel = row.substr(tab + 1);
    if (rel.empty()) continue;
    if (type == "d") {
      out.push_back(absl::StrCat("Directory: ", rel, "/"));
    } else {
      out.push_back(absl::StrCat("File: ", rel));
    }
  }
  return absl::StrJoin(out, "\n");
}

}  // namespace slop
