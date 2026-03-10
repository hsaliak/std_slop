#include "core/tool_executor.h"

#include <fstream>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

#include "core/status_macros.h"
#include "core/tools/common.h"
#include "core/json_utils.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandleWriteFile(const nlohmann::json& args) const {
  RETURN_IF_ERROR(MaybeEnforceMailStagingGuard(mail_mode_));

  auto path = json_get<std::string>(args, "path");
  if (!path) {
    return absl::InvalidArgumentError("Missing mandatory field: path");
  }
  auto content = json_get<std::string>(args, "content");
  if (!content) {
    return absl::InvalidArgumentError("Missing mandatory field: content");
  }

  if (absl::StrContains(*path, "..") || absl::StartsWith(*path, "/")) {
    return absl::PermissionDeniedError("SECURITY_VIOLATION: Path traversal (..) or absolute paths are not allowed.");
  }

  std::ofstream out(*path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return absl::InternalError("IO_ERROR: Failed to write to file");
  }
  out << *content;
  if (!out.good()) {
    return absl::InternalError("IO_ERROR: Failed to write to file");
  }
  out.close();

  return absl::StrCat("File written successfully:\nPath: ", *path, "\nBytes written: ", content->size(), "\n");
}

}  // namespace slop
