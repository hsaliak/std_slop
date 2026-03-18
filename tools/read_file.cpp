#include <fstream>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"

#include "core/json_utils.h"
#include "core/status_macros.h"
#include "tools/tool_executor.h"
#include "tools/common.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandleReadFile(const nlohmann::json& args) {
  RETURN_IF_ERROR(ValidateReadFileArgs(args));
  auto path = json_get<std::string>(args, "path");
  CHECK(path.has_value());

  ASSIGN_OR_RETURN(auto start_opt, ParseOptionalInteger(args, "start_line"));
  ASSIGN_OR_RETURN(auto end_opt, ParseOptionalInteger(args, "end_line"));

  std::ifstream in(*path);
  if (!in.is_open()) {
    return absl::NotFoundError(absl::StrCat("Could not open file: ", *path));
  }

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }

  const int start_line = start_opt.has_value() ? *start_opt : 1;
  const int end_line = end_opt.has_value() ? *end_opt : static_cast<int>(lines.size());
  if (start_line > static_cast<int>(lines.size())) {
    return absl::InvalidArgumentError(
        absl::StrCat("start_line (", start_line, ") exceeds file length (", lines.size(), ")"));
  }

  const bool line_numbers = json_get_or<bool>(args, "line_numbers", false);
  const int begin = std::max(1, start_line) - 1;
  const int end_exclusive = std::min(end_line, static_cast<int>(lines.size()));

  std::vector<std::string> out_lines;
  for (int i = begin; i < end_exclusive; ++i) {
    if (line_numbers) {
      out_lines.push_back(absl::StrCat(i + 1, ": ", lines[i]));
    } else {
      out_lines.push_back(lines[i]);
    }
  }

  if (out_lines.empty()) return std::string();
  return absl::StrCat(absl::StrJoin(out_lines, "\n"), "\n");
}

}  // namespace slop
