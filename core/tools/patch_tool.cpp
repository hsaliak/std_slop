#include "core/tool_executor.h"

#include <fstream>
#include <sstream>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"

#include "core/database.h"
#include "core/shell_util.h"
#include "core/status_macros.h"
#include "core/tool_dispatcher.h"
#include "core/tools/common.h"
#include "interface/color.h"
#include "interface/renderer.h"
#include "interface/terminal.h"
#include "core/json_utils.h"

namespace slop {
absl::StatusOr<std::string> ToolExecutor::HandlePatchTool(const nlohmann::json& args) const {
  RETURN_IF_ERROR(MaybeEnforceMailStagingGuard(mail_mode_));

  const nlohmann::json in = args.is_object() ? args : nlohmann::json::object();
  auto path = json_get<std::string>(in, "path");
  auto unified_diff = json_get<std::string>(in, "unified_diff");
  const bool dry_run = json_get_or<bool>(in, "dry_run", false);
  const bool ignore_whitespace = json_get_or<bool>(in, "ignore_whitespace", true);

  if (!path || path->empty()) {
    return nlohmann::json({{"ok", false}, {"code", "INVALID_ARGUMENT"}, {"error", {"message", "path is required"}}}).dump();
  }
  if (!unified_diff || unified_diff->empty()) {
    return nlohmann::json(
               {{"ok", false}, {"code", "INVALID_ARGUMENT"}, {"error", {"message", "unified_diff is required"}}})
        .dump();
  }
  if (absl::StrContains(*path, "..") || absl::StartsWith(*path, "/")) {
    return absl::PermissionDeniedError("SECURITY_VIOLATION: Path traversal (..) or absolute paths are not allowed.");
  }

  auto normalize_for_match = [&](const std::string& s) {
    // JS parity: normalization is always applied even when ignore_whitespace=false.
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
    lines.push_back(part);
    if (trailing_newline && !lines.empty()) lines.pop_back();
    return std::pair<std::vector<std::string>, bool>(lines, trailing_newline);
  };

  struct Op {
    char type;
    std::string text;
  };
  struct Hunk {
    std::string header;
    std::vector<Op> ops;
  };

  auto parse_unified_diff = [&](const std::string& diff_text) {
    std::vector<Hunk> hunks;
    const std::string src = normalize_newlines(diff_text);
    std::vector<std::string> lines;
    {
      std::string part;
      for (char c : src) {
        if (c == '\n') {
          lines.push_back(part);
          part.clear();
        } else {
          part.push_back(c);
        }
      }
      lines.push_back(part);
    }
    std::optional<Hunk> current;
    for (const auto& line : lines) {
      if (absl::StartsWith(line, "@@")) {
        if (current.has_value() && !current->ops.empty()) hunks.push_back(*current);
        current = Hunk{.header = line, .ops = {}};
        continue;
      }
      if (!current.has_value()) continue;
      if (line == "\\ No newline at end of file") continue;
      if (line.empty()) continue;
      const char prefix = line[0];
      const std::string body = line.substr(1);
      if (prefix == ' ' || prefix == '-' || prefix == '+') {
        current->ops.push_back(Op{.type = prefix, .text = body});
      }
    }
    if (current.has_value() && !current->ops.empty()) hunks.push_back(*current);
    return hunks;
  };

  const auto hunks = parse_unified_diff(*unified_diff);
  if (hunks.empty()) {
    return nlohmann::json({{"ok", false},
                           {"path", *path},
                           {"code", "PATCH_PARSE_FAILED"},
                           {"error", {{"message", "No valid hunks found in unified_diff"}}}})
        .dump();
  }

  std::ifstream file_in(*path, std::ios::binary);
  if (!file_in.is_open()) {
    return absl::NotFoundError(absl::StrCat("File not found: ", *path));
  }
  std::stringstream original_buf;
  original_buf << file_in.rdbuf();
  const std::string original_text = original_buf.str();
  auto split = split_lines(original_text);
  std::vector<std::string> working_lines = split.first;
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
      return nlohmann::json({{"ok", false},
                             {"path", *path},
                             {"code", "PATCH_DRY_RUN_FAILED"},
                             {"error", {{"message", "Unable to match hunk in target file"},
                                        {"detail", hunks[h].header},
                                        {"hunk_index", static_cast<int>(h)}}}})
          .dump();
    }
    working_lines.erase(working_lines.begin() + at, working_lines.begin() + at + static_cast<int>(old_lines.size()));
    working_lines.insert(working_lines.begin() + at, new_lines.begin(), new_lines.end());
    cursor = static_cast<size_t>(at) + new_lines.size();
  }

  if (dry_run) {
    return nlohmann::json({{"ok", true},
                           {"mode", "dry_run"},
                           {"path", *path},
                           {"can_apply", true},
                           {"applied", static_cast<int>(hunks.size())},
                           {"options", {{"ignore_whitespace", ignore_whitespace}}}})
        .dump();
  }

  std::string final_text = absl::StrJoin(working_lines, "\n");
  if (split.second) final_text.push_back('\n');
  std::ofstream out(*path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return absl::InternalError("IO_ERROR: Failed to write to file");
  }
  out << final_text;
  if (!out.good()) {
    return absl::InternalError("IO_ERROR: Failed to write to file");
  }

  return nlohmann::json({{"ok", true},
                         {"mode", "apply"},
                         {"path", *path},
                         {"applied", static_cast<int>(hunks.size())},
                         {"options", {{"ignore_whitespace", ignore_whitespace}}}})
      .dump();
}

}  // namespace slop
