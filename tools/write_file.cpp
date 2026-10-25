#include <filesystem>

#include "absl/status/status.h"

#include "core/json_utils.h"
#include "core/status_macros.h"
#include "tools/common.h"
#include "tools/file_operations.h"
#include "tools/tool_executor.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandleWriteFile(const nlohmann::json& args) const {
  RETURN_IF_ERROR(MaybeEnforceMailStagingGuard(mail_mode_));

  const auto path = json_get<std::string>(args, "path");
  if (!path) return absl::InvalidArgumentError("Missing mandatory field: path");
  const auto content = json_get<std::string>(args, "content");
  if (!content) return absl::InvalidArgumentError("Missing mandatory field: content");

  return file_ops::WriteWorkspaceFile(std::filesystem::path("."), *path, *content);
}

}  // namespace slop
