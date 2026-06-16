#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

#include "core/json_utils.h"
#include "core/status_macros.h"
#include "tools/common.h"
#include "tools/tool_executor.h"

namespace slop {
namespace {

enum class EditOp {
  kReplace,
  kInsertBefore,
  kInsertAfter,
  kDelete,
};

enum class WhichKind {
  kOnly,
  kFirst,
  kLast,
  kIndex,
};

struct Which {
  WhichKind kind = WhichKind::kOnly;
  size_t index = 0;
};

struct Edit {
  EditOp op;
  std::string find;
  std::string text;
  Which which;
};

std::string EditOpName(EditOp op) {
  switch (op) {
    case EditOp::kReplace:
      return "replace";
    case EditOp::kInsertBefore:
      return "insert_before";
    case EditOp::kInsertAfter:
      return "insert_after";
    case EditOp::kDelete:
      return "delete";
  }
}

absl::StatusOr<EditOp> ParseEditOp(const nlohmann::json& edit, int index) {
  auto op = json_get<std::string>(edit, "op");
  if (!op) {
    return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] missing mandatory field: op"));
  }
  if (*op == "replace") return EditOp::kReplace;
  if (*op == "insert_before") return EditOp::kInsertBefore;
  if (*op == "insert_after") return EditOp::kInsertAfter;
  if (*op == "delete") return EditOp::kDelete;
  return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] has unknown op: ", *op));
}

absl::StatusOr<Which> ParseWhich(const nlohmann::json& edit, int index) {
  const auto* which_json = json_at(edit, "which");
  if (which_json == nullptr || which_json->is_null()) return Which{};

  if (json_is<std::string>(*which_json)) {
    const std::string which = which_json->get<std::string>();
    if (which == "only") return Which{WhichKind::kOnly, 0};
    if (which == "first") return Which{WhichKind::kFirst, 0};
    if (which == "last") return Which{WhichKind::kLast, 0};
    return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] has invalid which: ", which));
  }

  if (json_is<nlohmann::json::number_integer_t>(*which_json)) {
    const int64_t which = which_json->get<int64_t>();
    if (which < 0) {
      return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] which index must be non-negative"));
    }
    if (static_cast<uint64_t>(which) > std::numeric_limits<size_t>::max()) {
      return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] which index is too large"));
    }
    return Which{WhichKind::kIndex, static_cast<size_t>(which)};
  }
  if (which_json->is_number_unsigned()) {
    const uint64_t which = which_json->get<uint64_t>();
    if (which > std::numeric_limits<size_t>::max()) {
      return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] which index is too large"));
    }
    return Which{WhichKind::kIndex, static_cast<size_t>(which)};
  }

  return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] which must be 'only', 'first', 'last', or a non-negative integer"));
}

absl::StatusOr<Edit> ParseEdit(const nlohmann::json& edit, int index) {
  if (!json_is<nlohmann::json::object_t>(edit)) {
    return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] must be an object"));
  }

  ASSIGN_OR_RETURN(EditOp op, ParseEditOp(edit, index));
  auto find = json_get<std::string>(edit, "find");
  if (!find) {
    return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] missing mandatory field: find"));
  }
  if (find->empty()) {
    return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] find must be non-empty"));
  }

  std::string text;
  const bool needs_text = op == EditOp::kReplace || op == EditOp::kInsertBefore || op == EditOp::kInsertAfter;
  auto text_or = json_get<std::string>(edit, "text");
  if (needs_text) {
    if (!text_or) {
      return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] missing mandatory field: text"));
    }
    text = *text_or;
    if (text.empty()) {
      return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] text must be non-empty"));
    }
  } else if (json_at(edit, "text") != nullptr) {
    return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] delete must not include text"));
  }

  if (op == EditOp::kReplace && *find == text) {
    return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] replace would be a no-op"));
  }

  ASSIGN_OR_RETURN(Which which, ParseWhich(edit, index));
  return Edit{op, *find, text, which};
}

std::vector<size_t> FindMatches(absl::string_view content, absl::string_view needle) {
  std::vector<size_t> matches;
  size_t pos = 0;
  while (pos < content.size()) {
    const size_t found = content.find(needle, pos);
    if (found == absl::string_view::npos) break;
    matches.push_back(found);
    pos = found + 1;
  }
  return matches;
}

absl::StatusOr<size_t> SelectMatch(const std::vector<size_t>& matches, const Which& which, int index) {
  if (matches.empty()) {
    return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] find text was not found"));
  }

  switch (which.kind) {
    case WhichKind::kOnly:
      if (matches.size() != 1) {
        return absl::InvalidArgumentError(
            absl::StrCat("edits[", index, "] expected exactly one match, found ", matches.size()));
      }
      return matches[0];
    case WhichKind::kFirst:
      return matches.front();
    case WhichKind::kLast:
      return matches.back();
    case WhichKind::kIndex:
      if (which.index >= matches.size()) {
        return absl::InvalidArgumentError(
            absl::StrCat("edits[", index, "] which index ", which.index, " is out of range for ", matches.size(), " matches"));
      }
      return matches[which.index];
  }
}

absl::Status ApplyEdit(std::string* content, const Edit& edit, int index, nlohmann::json* summary) {
  const std::vector<size_t> matches = FindMatches(*content, edit.find);
  ASSIGN_OR_RETURN(size_t at, SelectMatch(matches, edit.which, index));

  size_t removed = 0;
  size_t inserted = 0;
  switch (edit.op) {
    case EditOp::kReplace:
      removed = edit.find.size();
      inserted = edit.text.size();
      content->replace(at, edit.find.size(), edit.text);
      break;
    case EditOp::kInsertBefore:
      inserted = edit.text.size();
      content->insert(at, edit.text);
      break;
    case EditOp::kInsertAfter:
      inserted = edit.text.size();
      content->insert(at + edit.find.size(), edit.text);
      break;
    case EditOp::kDelete:
      removed = edit.find.size();
      content->erase(at, edit.find.size());
      break;
  }

  summary->push_back(nlohmann::json{{"i", index},
                                    {"op", EditOpName(edit.op)},
                                    {"at", at},
                                    {"matches", matches.size()},
                                    {"old", removed},
                                    {"new", inserted}});
  return absl::OkStatus();
}

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
  if (!path) {
    return absl::InvalidArgumentError("Missing mandatory field: path");
  }
  if (absl::StrContains(*path, "..") || absl::StartsWith(*path, "/")) {
    return absl::PermissionDeniedError("SECURITY_VIOLATION: Path traversal (..) or absolute paths are not allowed.");
  }

  const auto* edits_json = json_at(args, "edits");
  if (edits_json == nullptr) {
    return absl::InvalidArgumentError("Missing mandatory field: edits");
  }
  if (!json_is<nlohmann::json::array_t>(*edits_json)) {
    return absl::InvalidArgumentError("edits must be an array");
  }
  if (edits_json->empty()) {
    return absl::InvalidArgumentError("edits must be non-empty");
  }

  std::vector<Edit> edits;
  edits.reserve(edits_json->size());
  for (size_t i = 0; i < edits_json->size(); ++i) {
    ASSIGN_OR_RETURN(Edit edit, ParseEdit((*edits_json)[i], static_cast<int>(i)));
    edits.push_back(std::move(edit));
  }

  std::ifstream in(*path, std::ios::binary);
  if (!in.is_open()) {
    return absl::NotFoundError(absl::StrCat("Could not open file: ", *path));
  }
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (!in.good() && !in.eof()) {
    return absl::InternalError("IO_ERROR: Failed to read file");
  }

  const size_t bytes_before = content.size();
  std::string working = content;
  nlohmann::json changes = nlohmann::json::array();
  for (size_t i = 0; i < edits.size(); ++i) {
    RETURN_IF_ERROR(ApplyEdit(&working, edits[i], static_cast<int>(i), &changes));
  }

  if (working == content) {
    return absl::InvalidArgumentError("edits would not change file content");
  }

  RETURN_IF_ERROR(WriteFileAtomically(*path, working));

  return json_dump(nlohmann::json{{"path", *path},
                                  {"edits", static_cast<int>(edits.size())},
                                  {"bytes_before", bytes_before},
                                  {"bytes_after", working.size()},
                                  {"changes", changes}});
}

}  // namespace slop
