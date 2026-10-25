#include "tools/edit_engine.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "core/status_macros.h"

namespace slop {
namespace {

constexpr std::size_t kMaxTextEditCount = 256;

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
  std::size_t index = 0;
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
  return "unknown";
}

absl::StatusOr<EditOp> ParseEditOp(const nlohmann::json& edit, std::size_t index) {
  auto op = json_get<std::string>(edit, "op");
  if (!op) return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] missing mandatory field: op"));
  if (*op == "replace") return EditOp::kReplace;
  if (*op == "insert_before") return EditOp::kInsertBefore;
  if (*op == "insert_after") return EditOp::kInsertAfter;
  if (*op == "delete") return EditOp::kDelete;
  return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] has unknown op: ", *op));
}

absl::StatusOr<Which> ParseWhich(const nlohmann::json& edit, std::size_t index) {
  const auto which_value = json_get<std::optional<nlohmann::json>>(edit, "which");
  if (!which_value || !which_value->has_value()) return Which{};
  const nlohmann::json& which_json = **which_value;

  if (const auto which = json_getter<std::string>::get(which_json)) {
    if (*which == "only") return Which{WhichKind::kOnly, 0};
    if (*which == "first") return Which{WhichKind::kFirst, 0};
    if (*which == "last") return Which{WhichKind::kLast, 0};
    return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] has invalid which: ", *which));
  }

  if (const auto which = json_getter<std::int64_t>::get(which_json)) {
    if (*which < 0) {
      return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] which index must be non-negative"));
    }
    if (static_cast<std::uint64_t>(*which) > std::numeric_limits<std::size_t>::max()) {
      return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] which index is too large"));
    }
    return Which{WhichKind::kIndex, static_cast<std::size_t>(*which)};
  }
  if (const auto which = json_getter<std::uint64_t>::get(which_json)) {
    if (*which > std::numeric_limits<std::size_t>::max()) {
      return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] which index is too large"));
    }
    return Which{WhichKind::kIndex, static_cast<std::size_t>(*which)};
  }

  return absl::InvalidArgumentError(
      absl::StrCat("edits[", index, "] which must be 'only', 'first', 'last', or a non-negative integer"));
}

absl::StatusOr<Edit> ParseEdit(const nlohmann::json& edit, std::size_t index) {
  if (!json_is<nlohmann::json::object_t>(edit)) {
    return absl::InvalidArgumentError(absl::StrCat("edits[", index, "] must be an object"));
  }

  ASSIGN_OR_RETURN(const EditOp op, ParseEditOp(edit, index));
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

  ASSIGN_OR_RETURN(const Which which, ParseWhich(edit, index));
  return Edit{op, *find, text, which};
}

absl::StatusOr<std::vector<Edit>> ParseEdits(const nlohmann::json& edits_json) {
  if (!json_is<nlohmann::json::array_t>(edits_json)) {
    return absl::InvalidArgumentError("edits must be an array");
  }
  if (edits_json.empty()) return absl::InvalidArgumentError("edits must be non-empty");
  if (edits_json.size() > kMaxTextEditCount) {
    return absl::ResourceExhaustedError(absl::StrCat("edits may contain at most ", kMaxTextEditCount, " items"));
  }

  std::vector<Edit> edits;
  edits.reserve(edits_json.size());
  for (std::size_t index = 0; index < edits_json.size(); ++index) {
    ASSIGN_OR_RETURN(Edit edit, ParseEdit(edits_json[index], index));
    edits.push_back(std::move(edit));
  }
  return edits;
}

std::vector<std::size_t> FindMatches(absl::string_view content, absl::string_view needle) {
  std::vector<std::size_t> matches;
  std::size_t position = 0;
  while (position < content.size()) {
    const std::size_t found = content.find(needle, position);
    if (found == absl::string_view::npos) break;
    matches.push_back(found);
    position = found + 1;
  }
  return matches;
}

absl::StatusOr<std::size_t> SelectMatch(const std::vector<std::size_t>& matches, const Which& which,
                                        std::size_t edit_index) {
  if (matches.empty()) {
    return absl::InvalidArgumentError(absl::StrCat("edits[", edit_index, "] find text was not found"));
  }

  switch (which.kind) {
    case WhichKind::kOnly:
      if (matches.size() != 1) {
        return absl::InvalidArgumentError(
            absl::StrCat("edits[", edit_index, "] expected exactly one match, found ", matches.size()));
      }
      return matches[0];
    case WhichKind::kFirst:
      return matches.front();
    case WhichKind::kLast:
      return matches.back();
    case WhichKind::kIndex:
      if (which.index >= matches.size()) {
        return absl::InvalidArgumentError(absl::StrCat("edits[", edit_index, "] which index ", which.index,
                                                       " is out of range for ", matches.size(), " matches"));
      }
      return matches[which.index];
  }
  return absl::InvalidArgumentError(absl::StrCat("edits[", edit_index, "] has an invalid match selector"));
}

absl::Status ApplyEdit(std::string* content, const Edit& edit, std::size_t index, nlohmann::json* summary) {
  const std::vector<std::size_t> matches = FindMatches(*content, edit.find);
  ASSIGN_OR_RETURN(const std::size_t at, SelectMatch(matches, edit.which, index));

  std::size_t removed = 0;
  std::size_t inserted = 0;
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

}  // namespace

absl::Status ValidateTextEdits(const nlohmann::json& edits) {
  auto parsed = ParseEdits(edits);
  return parsed.ok() ? absl::OkStatus() : parsed.status();
}

absl::StatusOr<TextEditResult> ApplyTextEdits(absl::string_view original, const nlohmann::json& edits_json) {
  ASSIGN_OR_RETURN(const std::vector<Edit> edits, ParseEdits(edits_json));
  std::string working(original);
  nlohmann::json changes = nlohmann::json::array();
  for (std::size_t index = 0; index < edits.size(); ++index) {
    RETURN_IF_ERROR(ApplyEdit(&working, edits[index], index, &changes));
  }
  if (working == original) return absl::InvalidArgumentError("edits would not change file content");
  return TextEditResult{std::move(working), std::move(changes), edits.size(), original.size()};
}

}  // namespace slop
