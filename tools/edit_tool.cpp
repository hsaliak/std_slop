#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

#include "core/json_utils.h"
#include "core/status_macros.h"
#include "tools/common.h"
#include "tools/edit_engine.h"
#include "tools/edit_engine.h"
#include "tools/tool_executor.h"

namespace slop {
namespace {

absl::Status WriteFileAtomically(const std::string& path, absl::string_view content) {
  std::string temp_path;
  for (int i = 0; i < 100; ++i) {
    const std::string candidate = absl::StrCat(path, ".edit_tool_tmp.", i);
    std::error_code exists_error;
    if (!std::filesystem::exists(candidate, exists_error)) {
      temp_path = candidate;
      break;
    }
  }
  if (temp_path.empty()) {
    return absl::AlreadyExistsError("Could not allocate temporary edit file");
  }

  std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) return absl::InternalError("IO_ERROR: Failed to write to temporary file");
  out << content;
  out.close();
  if (!out.good()) return absl::InternalError("IO_ERROR: Failed to write to temporary file");

  std::error_code rename_error;
  std::filesystem::rename(temp_path, path, rename_error);
  if (rename_error) {
    std::error_code remove_error;
    std::filesystem::remove(temp_path, remove_error);
    return absl::InternalError(absl::StrCat("IO_ERROR: Failed to replace file: ", rename_error.message()));
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<std::string> ToolExecutor::HandleEditTool(const nlohmann::json& args) const {
  RETURN_IF_ERROR(MaybeEnforceMailStagingGuard(mail_mode_));

  if (!json_is<nlohmann::json::object_t>(args)) {
    return absl::InvalidArgumentError("edit_tool args must be an object");
  }

  auto path = json_get<std::string>(args, "path");
  if (!path) return absl::InvalidArgumentError("Missing mandatory field: path");
  if (absl::StrContains(*path, "..") || absl::StartsWith(*path, "/")) {
    return absl::PermissionDeniedError("SECURITY_VIOLATION: Path traversal (..) or absolute paths are not allowed.");
  }

  const auto* edits = json_at(args, "edits");
  if (edits == nullptr) return absl::InvalidArgumentError("Missing mandatory field: edits");
  RETURN_IF_ERROR(ValidateTextEdits(*edits));

  std::ifstream in(*path, std::ios::binary);
  if (!in.is_open()) return absl::NotFoundError(absl::StrCat("Could not open file: ", *path));
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (!in.good() && !in.eof()) return absl::InternalError("IO_ERROR: Failed to read file");

  ASSIGN_OR_RETURN(TextEditResult edited, ApplyTextEdits(content, *edits));
  RETURN_IF_ERROR(WriteFileAtomically(*path, edited.content));

  return json_dump(nlohmann::json{{"path", *path},
                                  {"edits", edited.edit_count},
                                  {"bytes_before", edited.bytes_before},
                                  {"bytes_after", edited.content.size()},
                                  {"changes", std::move(edited.changes)}});
}

}  // namespace slop
