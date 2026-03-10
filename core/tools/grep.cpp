#include "core/tool_executor.h"

#include <sstream>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"

#include "core/shell_util.h"
#include "core/status_macros.h"
#include "core/tools/common.h"
#include "core/json_utils.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandleGrep(const nlohmann::json& args) {
  auto pattern = json_get<std::string>(args, "pattern");
  if (!pattern || pattern->empty()) {
    return absl::InvalidArgumentError("Missing mandatory field: pattern");
  }

  const std::string search_path = json_get_or<std::string>(args, "path", ".");

  ASSIGN_OR_RETURN(auto context_opt, ParseOptionalInteger(args, "context"));
  ASSIGN_OR_RETURN(auto limit_opt, ParseOptionalInteger(args, "limit"));
  const int context = std::max(0, context_opt.value_or(0));
  const int limit = std::max(1, limit_opt.value_or(200));

  std::vector<std::string> ignore_dirs;
  std::vector<std::string> ignore_files;
  const bool include_ignored = json_get_or<bool>(args, "include_ignored", false);
  if (!include_ignored) {
    ignore_dirs = {".git", "node_modules", "bazel-*", "dist", "build", ".cache", "target"};
  }

  if (const auto* ignore = json_at(args, "ignore")) {
    if (ignore->is_string() && !ignore->get<std::string>().empty()) {
      ignore_dirs.push_back(ignore->get<std::string>());
    } else if (ignore->is_array()) {
      for (const auto& v : *ignore) {
        if (v.is_string() && !v.get<std::string>().empty()) {
          ignore_dirs.push_back(v.get<std::string>());
        }
      }
    }
  }

  // Minimal root .gitignore support parity: plain dir/filename entries only.
  int skipped_gitignore_rules = 0;
  if (!include_ignored) {
    auto root_or = RunCommand("git rev-parse --show-toplevel 2>/dev/null");
    if (root_or.ok() && root_or->exit_code == 0) {
      std::string repo_root = TrimNewlines(root_or->stdout_out);
      if (!repo_root.empty()) {
        auto cat_or = RunCommand(absl::StrCat("cat ", EscapeShellArg(absl::StrCat(repo_root, "/.gitignore")), " 2>/dev/null"));
        if (cat_or.ok() && cat_or->exit_code == 0) {
          std::stringstream gs(cat_or->stdout_out);
          std::string line;
          while (std::getline(gs, line)) {
            std::string t(absl::StripAsciiWhitespace(line));
            if (t.empty() || t[0] == '#') continue;
            if (t[0] == '!' || absl::StrContains(t, "**")) {
              skipped_gitignore_rules++;
              continue;
            }
            if (!t.empty() && t.back() == '/') {
              t.pop_back();
              t = absl::StripAsciiWhitespace(t);
              if (t.empty() || absl::StrContains(t, "/")) {
                skipped_gitignore_rules++;
              } else {
                ignore_dirs.push_back(t);
              }
              continue;
            }
            if (absl::StrContains(t, "/")) {
              skipped_gitignore_rules++;
            } else {
              ignore_files.push_back(t);
            }
          }
        }
      }
    }
  }

  std::vector<std::string> exclude_args;
  exclude_args.reserve(ignore_dirs.size() + ignore_files.size());
  for (const auto& d : ignore_dirs) exclude_args.push_back(absl::StrCat("--exclude-dir=", EscapeShellArg(d)));
  for (const auto& f : ignore_files) exclude_args.push_back(absl::StrCat("--exclude=", EscapeShellArg(f)));

  const bool fixed_strings = json_get_or<bool>(args, "fixed_strings", false);
  const std::string mode_flag = fixed_strings ? "-F" : "-E";
  const std::string context_arg = context > 0 ? absl::StrCat(" -C ", context) : "";
  const std::string cmd = absl::StrCat("grep -rn ", mode_flag, " -I --color=never", context_arg,
                                       (exclude_args.empty() ? "" : absl::StrCat(" ", absl::StrJoin(exclude_args, " "))),
                                       " -e ", EscapeShellArg(*pattern), " ", EscapeShellArg(search_path));

  ASSIGN_OR_RETURN(auto res, RunCommand(cmd));
  if (res.exit_code != 0 && res.exit_code != 1) {
    return absl::InternalError(
        absl::StrCat("INTERNAL: Command failed with status ", res.exit_code, "\nOutput:\n", res.stdout_out, res.stderr_out));
  }

  std::string output = res.stdout_out;
  std::vector<std::string> lines;
  if (!output.empty()) {
    std::stringstream os(output);
    std::string l;
    while (std::getline(os, l)) lines.push_back(l);
  }
  if (static_cast<int>(lines.size()) > limit) {
    output = absl::StrCat(absl::StrJoin(std::vector<std::string>(lines.begin(), lines.begin() + limit), "\n"),
                          "\n[TRUNCATED: Use a more specific pattern or path to narrow results]");
  }
  if (skipped_gitignore_rules > 0) {
    if (!output.empty() && output.back() == '\n') output.pop_back();
    output = absl::StrCat(output, (output.empty() ? "" : "\n"), "[NOTE: Skipped ", skipped_gitignore_rules,
                          " unsupported root .gitignore rule(s)]");
  }
  return output;
}

}  // namespace slop
