#include <filesystem>
#include <string>

#include "absl/status/status.h"

#include "core/json_utils.h"
#include "core/status_macros.h"
#include "tools/common.h"
#include "tools/file_operations.h"
#include "tools/tool_executor.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandleReadFile(const nlohmann::json& args) {
  RETURN_IF_ERROR(ValidateReadFileArgs(args));
  const auto path = json_get<std::string>(args, "path");
  CHECK(path.has_value());
  ASSIGN_OR_RETURN(const auto start_line, ParseOptionalInteger(args, "start_line"));
  ASSIGN_OR_RETURN(const auto end_line, ParseOptionalInteger(args, "end_line"));
  const bool line_numbers = json_get_or<bool>(args, "line_numbers", false);
  return file_ops::ReadWorkspaceFile(std::filesystem::path("."), *path,
                                     {start_line, end_line, line_numbers});
}

}  // namespace slop
