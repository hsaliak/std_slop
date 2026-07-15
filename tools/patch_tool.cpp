#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"

#include "core/json_utils.h"
#include "core/status_macros.h"
#include "tools/tool_executor.h"
#include "tools/common.h"

namespace slop {
namespace {

bool IsWithinRepository(const std::filesystem::path& repository_root, const std::filesystem::path& target) {
  auto root_it = repository_root.begin();
  auto target_it = target.begin();
  for (; root_it != repository_root.end(); ++root_it, ++target_it) {
    if (target_it == target.end() || *root_it != *target_it) return false;
  }
  return true;
}

}  // namespace

absl::StatusOr<std::string> ToolExecutor::HandlePatchTool(const nlohmann::json& args) const {
  RETURN_IF_ERROR(MaybeEnforceMailStagingGuard(mail_mode_));

  const nlohmann::json in = args.is_object() ? args : nlohmann::json::object();
  auto path = json_get<std::string>(in, "path");
  auto unified_diff = json_get<std::string>(in, "unified_diff");
  const bool dry_run = json_get_or<bool>(in, "dry_run", false);
  const bool ignore_whitespace = json_get_or<bool>(in, "ignore_whitespace", true);

  if (!path || path->empty()) {
    return json_dump(
        nlohmann::json({{"ok", false}, {"code", "INVALID_ARGUMENT"}, {"error", {"message", "path is required"}}}));
  }
  if (!unified_diff || unified_diff->empty()) {
    return json_dump(
        nlohmann::json(
            {{"ok", false}, {"code", "INVALID_ARGUMENT"}, {"error", {"message", "unified_diff is required"}}}));
  }
  if (absl::StrContains(*path, "..") || absl::StartsWith(*path, "/")) {
    return absl::PermissionDeniedError("SECURITY_VIOLATION: Path traversal (..) or absolute paths are not allowed.");
  }
  std::error_code error;
  const std::filesystem::path repository_root = std::filesystem::canonical(std::filesystem::current_path(), error);
  if (error) return absl::InternalError("IO_ERROR: Failed to resolve repository root");
  const std::filesystem::path target_path = std::filesystem::canonical(*path, error);
  if (error || !IsWithinRepository(repository_root, target_path)) {
    return absl::PermissionDeniedError("SECURITY_VIOLATION: Patch target must resolve within the repository");
  }

  auto normalize_for_match = [&](const std::string& s) {
    if (!ignore_whitespace) return s;
    std::string out;
    out.reserve(s.size());
    bool prev_ws = false;
    for (char c : s) {
      const bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v');
      if (ws) {
        if (!prev_ws) out.push_back(' ');
      } else {
        out.push_back(c);
      }
      prev_ws = ws;
    }
    size_t start = 0;
    while (start < out.size() && out[start] == ' ') start++;
    size_t end = out.size();
    while (end > start && out[end - 1] == ' ') end--;
    return out.substr(start, end - start);
  };

  auto normalize_newlines = [](std::string s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
      const char c = s[i];
      if (c == '\r') {
        if (i + 1 < s.size() && s[i + 1] == '\n') {
          continue;
        }
        out.push_back('\n');
      } else {
        out.push_back(c);
      }
    }
    return out;
  };

  auto split_lines = [&](const std::string& text) {
    const std::string normalized = normalize_newlines(text);
    const bool trailing_newline = !normalized.empty() && normalized.back() == '\n';
    std::vector<std::string> lines;
    std::string part;
    for (char c : normalized) {
      if (c == '\n') {
        lines.push_back(part);
        part.clear();
      } else {
        part.push_back(c);
      }
    }
    if (!part.empty()) lines.push_back(part);
    return std::pair<std::vector<std::string>, bool>(lines, trailing_newline);
  };

  struct Op {
    char type;
    std::string text;
  };
  struct Hunk {
    std::string header;
    int old_count;
    int new_count;
    std::vector<Op> ops;
    std::optional<bool> trailing_newline;
  };

  auto parse_unified_diff = [&](const std::string& diff_text) -> absl::StatusOr<std::vector<Hunk>> {
    auto valid_range = [](absl::string_view range) {
      if (range.empty()) return false;
      size_t index = 0;
      while (index < range.size() && std::isdigit(static_cast<unsigned char>(range[index]))) ++index;
      if (index == 0) return false;
      if (index == range.size()) return true;
      if (range[index] != ',') return false;
      ++index;
      const size_t count_start = index;
      while (index < range.size() && std::isdigit(static_cast<unsigned char>(range[index]))) ++index;
      return count_start != index && index == range.size();
    };
    auto range_count = [&](absl::string_view range) -> absl::StatusOr<int> {
      if (!valid_range(range)) return absl::InvalidArgumentError("Invalid unified diff hunk header");
      const size_t comma = range.find(',');
      if (comma == absl::string_view::npos) return 1;
      int count = 0;
      if (!absl::SimpleAtoi(range.substr(comma + 1), &count) || count < 0) {
        return absl::InvalidArgumentError("Invalid unified diff hunk header");
      }
      return count;
    };
    auto parse_header = [&](absl::string_view header) -> absl::StatusOr<std::pair<int, int>> {
      if (!absl::StartsWith(header, "@@ -")) return absl::InvalidArgumentError("Invalid unified diff hunk header");
      const size_t separator = header.find(" +", 4);
      if (separator == absl::string_view::npos) return absl::InvalidArgumentError("Invalid unified diff hunk header");
      const size_t ending = header.find(" @@", separator + 2);
      if (ending == absl::string_view::npos) return absl::InvalidArgumentError("Invalid unified diff hunk header");
      ASSIGN_OR_RETURN(const int old_count, range_count(header.substr(4, separator - 4)));
      ASSIGN_OR_RETURN(const int new_count, range_count(header.substr(separator + 2, ending - separator - 2)));
      return std::pair<int, int>(old_count, new_count);
    };

    std::vector<Hunk> hunks;
    const std::string src = normalize_newlines(diff_text);
    std::vector<std::string> lines;
    std::string part;
    for (char c : src) {
      if (c == '\n') {
        lines.push_back(part);
        part.clear();
      } else {
        part.push_back(c);
      }
    }
    if (!part.empty()) lines.push_back(part);

    std::optional<Hunk> current;
    for (const auto& line : lines) {
      if (absl::StartsWith(line, "@@")) {
        ASSIGN_OR_RETURN(const auto counts, parse_header(line));
        if (current.has_value()) {
          if (current->ops.empty()) return absl::InvalidArgumentError("Unified diff hunk has no operations");
          hunks.push_back(*current);
        }
        current = Hunk{line, counts.first, counts.second, {}, std::nullopt};
        continue;
      }
      if (!current.has_value()) continue;
      if (line == "\\ No newline at end of file") {
        if (current->ops.empty()) {
          return absl::InvalidArgumentError("No-newline marker must follow a hunk operation");
        }
        current->trailing_newline = current->ops.back().type == '-';
        continue;
      }
      if (line.empty() || (line[0] != ' ' && line[0] != '-' && line[0] != '+')) {
        return absl::InvalidArgumentError("Invalid unified diff hunk body line");
      }
      current->ops.push_back(Op{line[0], line.substr(1)});
    }
    if (current.has_value()) {
      if (current->ops.empty()) return absl::InvalidArgumentError("Unified diff hunk has no operations");
      hunks.push_back(*current);
    }
    if (hunks.empty()) return absl::InvalidArgumentError("No valid hunks found in unified_diff");
    for (const Hunk& hunk : hunks) {
      int old_count = 0;
      int new_count = 0;
      for (const Op& op : hunk.ops) {
        if (op.type != '+') ++old_count;
        if (op.type != '-') ++new_count;
      }
      if (old_count != hunk.old_count || new_count != hunk.new_count) {
        return absl::InvalidArgumentError("Unified diff hunk line counts do not match its header");
      }
    }
    return hunks;
  };

  auto hunks_or = parse_unified_diff(*unified_diff);
  if (!hunks_or.ok()) {
    return json_dump(nlohmann::json({{"ok", false},
                                     {"path", *path},
                                     {"code", "PATCH_PARSE_FAILED"},
                                     {"error", {{"message", hunks_or.status().message()}}}}));
  }
  const auto& hunks = *hunks_or;

  std::ifstream file_in(target_path, std::ios::binary);
  if (!file_in.is_open()) {
    return absl::NotFoundError(absl::StrCat("File not found: ", *path));
  }
  std::stringstream original_buf;
  original_buf << file_in.rdbuf();
  const std::string original_text = original_buf.str();
  auto split = split_lines(original_text);
  std::vector<std::string> working_lines = split.first;
  bool trailing_newline = split.second;
  size_t cursor = 0;

  auto old_from_ops = [](const std::vector<Op>& ops) {
    std::vector<std::string> out;
    for (const auto& op : ops)
      if (op.type != '+') out.push_back(op.text);
    return out;
  };
  auto new_from_ops = [](const std::vector<Op>& ops) {
    std::vector<std::string> out;
    for (const auto& op : ops)
      if (op.type != '-') out.push_back(op.text);
    return out;
  };

  auto find_hunk_start = [&](const std::vector<std::string>& file_lines, const std::vector<std::string>& old_lines,
                             size_t start_at) {
    const size_t n = file_lines.size();
    const size_t m = old_lines.size();
    if (m == 0) return static_cast<int>(n);
    auto matches_at = [&](size_t idx) {
      if (idx + m > n) return false;
      for (size_t j = 0; j < m; ++j) {
        if (normalize_for_match(file_lines[idx + j]) != normalize_for_match(old_lines[j])) {
          return false;
        }
      }
      return true;
    };
    for (size_t i = std::max<size_t>(0, start_at); i + m <= n; ++i) {
      if (matches_at(i)) return static_cast<int>(i);
    }
    for (size_t i = 0; i < std::max<size_t>(0, start_at) && i + m <= n; ++i) {
      if (matches_at(i)) return static_cast<int>(i);
    }
    return -1;
  };

  for (size_t h = 0; h < hunks.size(); ++h) {
    const auto old_lines = old_from_ops(hunks[h].ops);
    const auto new_lines = new_from_ops(hunks[h].ops);
    const int at = find_hunk_start(working_lines, old_lines, cursor);
    if (at < 0) {
      return json_dump(nlohmann::json({{"ok", false},
                                       {"path", *path},
                                       {"code", "PATCH_DRY_RUN_FAILED"},
                                       {"unified_diff", *unified_diff},
                                       {"error",
                                        {{"message", "Unable to match hunk in target file"},
                                         {"detail", hunks[h].header},
                                         {"hunk_index", static_cast<int>(h)}}}}));
    }
    working_lines.erase(working_lines.begin() + at, working_lines.begin() + at + static_cast<int>(old_lines.size()));
    working_lines.insert(working_lines.begin() + at, new_lines.begin(), new_lines.end());
    if (hunks[h].trailing_newline.has_value()) trailing_newline = *hunks[h].trailing_newline;
    cursor = static_cast<size_t>(at) + new_lines.size();
  }

  if (dry_run) {
    return json_dump(nlohmann::json({{"ok", true},
                                     {"mode", "dry_run"},
                                     {"path", *path},
                                     {"can_apply", true},
                                     {"applied", static_cast<int>(hunks.size())},
                                     {"unified_diff", *unified_diff},
                                     {"options", {{"ignore_whitespace", ignore_whitespace}}}}));
  }

  std::string final_text = absl::StrJoin(working_lines, "\n");
  if (trailing_newline && !final_text.empty()) final_text.push_back('\n');
  std::ofstream out(target_path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return absl::InternalError("IO_ERROR: Failed to write to file");
  }
  out << final_text;
  if (!out.good()) {
    return absl::InternalError("IO_ERROR: Failed to write to file");
  }

  return json_dump(nlohmann::json({{"ok", true},
                                   {"mode", "apply"},
                                   {"path", *path},
                                   {"applied", static_cast<int>(hunks.size())},
                                   {"unified_diff", *unified_diff},
                                   {"options", {{"ignore_whitespace", ignore_whitespace}}}}));
}

}  // namespace slop
