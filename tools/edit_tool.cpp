#include <filesystem>
#include <string>
#include <utility>

#include "absl/status/status.h"

#include "core/json_utils.h"
#include "core/status_macros.h"
#include "tools/common.h"
#include "tools/file_operations.h"
#include "tools/tool_executor.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandleEditTool(const nlohmann::json& args) const {
  RETURN_IF_ERROR(MaybeEnforceMailStagingGuard(mail_mode_));

  if (!json_is<nlohmann::json::object_t>(args)) {
    return absl::InvalidArgumentError("edit_tool args must be an object");
  }

  const auto path = json_get<std::string>(args, "path");
  if (!path) return absl::InvalidArgumentError("Missing mandatory field: path");
  const nlohmann::json* edits = json_at(args, "edits");
  if (edits == nullptr) return absl::InvalidArgumentError("Missing mandatory field: edits");

  ASSIGN_OR_RETURN(TextEditResult edited, file_ops::EditWorkspaceFile(std::filesystem::path("."), *path, *edits));
  return json_dump(nlohmann::json{{"path", *path},
                                  {"edits", edited.edit_count},
                                  {"bytes_before", edited.bytes_before},
                                  {"bytes_after", edited.content.size()},
                                  {"changes", std::move(edited.changes)}});
}

}  // namespace slop
