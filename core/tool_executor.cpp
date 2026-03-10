#include "core/tool_executor.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"

#include "core/database.h"
#include "core/js_preamble_data.h"
#include "core/shell_util.h"
#include "core/status_macros.h"
#include "core/tool_dispatcher.h"
#include "interface/color.h"
#include "interface/renderer.h"
#include "interface/terminal.h"
#include "js-bridge/interpreter.h"
#include "json_utils.h"

namespace slop {
namespace {

bool IsDebugToolsEnabled() { return std::getenv("SLOP_DEBUG_TOOLS") != nullptr; }

std::string JsonKeys(const nlohmann::json& j) {
  if (!j.is_object()) return "<non-object>";
  std::vector<std::string> keys;
  keys.reserve(j.size());
  for (auto it = j.begin(); it != j.end(); ++it) {
    keys.push_back(it.key());
  }
  return keys.empty() ? "<empty>" : absl::StrJoin(keys, ",");
}

std::string TruncateForLog(const std::string& s, size_t max_len = 240) {
  if (s.size() <= max_len) return s;
  return s.substr(0, max_len) + "...";
}

absl::StatusOr<std::optional<int>> ParseOptionalInteger(const nlohmann::json& args, const std::string& key) {
  const auto* v = json_at(args, key);
  if (!v || v->is_null()) return std::optional<int>();
  if (v->is_number_integer()) return v->get<int>();
  if (v->is_string()) {
    int parsed = 0;
    if (absl::SimpleAtoi(v->get<std::string>(), &parsed)) {
      return parsed;
    }
  }
  return absl::InvalidArgumentError(absl::StrCat(key, " must be an integer"));
}

std::string TrimNewlines(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

absl::StatusOr<std::string> GetCurrentBranchName() {
  const char* forced = std::getenv("SLOP_FORCE_BRANCH_NAME");
  if (forced && *forced) {
    return std::string(forced);
  }
  ASSIGN_OR_RETURN(auto res, RunCommand("git branch --show-current"));
  if (res.exit_code != 0) {
    return absl::InternalError(absl::StrCat("Failed to get current branch: ", res.stderr_out));
  }
  return TrimNewlines(res.stdout_out);
}

absl::Status MaybeEnforceMailStagingGuard(bool mail_mode) {
  if (!mail_mode) return absl::OkStatus();
  const char* v = std::getenv("SLOP_SKIP_STAGING_CHECK");
  if (v && std::string(v) == "1") return absl::OkStatus();

  ASSIGN_OR_RETURN(const std::string branch, GetCurrentBranchName());
  if (!absl::StartsWith(branch, "slop/staging/") && branch != "HEAD") {
    return absl::FailedPreconditionError(
        absl::StrCat("Destructive operations are only allowed on 'slop/staging/*' branches. Current branch: ", branch));
  }
  return absl::OkStatus();
}

absl::StatusOr<nlohmann::json> ParseDbRows(const std::string& rows_json, const std::string& context) {
  auto parsed = nlohmann::json::parse(rows_json, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_array()) {
    return absl::InternalError(absl::StrCat("Failed to parse ", context));
  }
  return parsed;
}

absl::StatusOr<std::string> ResolveBaseBranch(Database* db, const std::string& requested_base) {
  if (!requested_base.empty()) return requested_base;
  ASSIGN_OR_RETURN(const std::string current, GetCurrentBranchName());
  if (current.empty()) return std::string("main");
  if (!db) return std::string("main");
  ASSIGN_OR_RETURN(auto rows_json,
                   db->Query("SELECT parent_branch FROM staging_branches WHERE branch_name = ?", {current}));
  ASSIGN_OR_RETURN(auto rows, ParseDbRows(rows_json, "staging branch lookup"));
  if (!rows.empty() && rows[0].is_object() && rows[0].contains("parent_branch") && rows[0]["parent_branch"].is_string()) {
    return rows[0]["parent_branch"].get<std::string>();
  }
  if (absl::StartsWith(current, "slop/staging/")) {
    return absl::InternalError(
        absl::StrCat("Base branch not found in database for staging branch '", current, "'."));
  }
  return std::string("main");
}

absl::Status AssertCleanWorkspace() {
  ASSIGN_OR_RETURN(auto res, RunCommand("git status --porcelain"));
  if (!res.stdout_out.empty()) {
    return absl::FailedPreconditionError(
        "Working tree is dirty. Please commit, stash, or discard changes before finalizing.");
  }
  return absl::OkStatus();
}

std::string CanonicalStagingBranch(const std::string& branch) {
  std::string out = branch;
  const std::string kPrefix = "slop/staging/";
  while (absl::StartsWith(out, kPrefix)) {
    out = out.substr(kPrefix.size());
  }
  return absl::StrCat(kPrefix, out);
}

}  // namespace

ToolExecutor::ToolExecutor(Database* db) : db_(db) { RegisterTools(); }

ToolExecutor::~ToolExecutor() = default;

void ToolExecutor::SetDispatcher(std::unique_ptr<ToolDispatcher> dispatcher) { dispatcher_ = std::move(dispatcher); }

void ToolExecutor::RegisterTool(const std::string& name, ToolHandler handler) {
  CHECK(!dispatch_map_.contains(name)) << "Duplicate tool registration: " << name;
  dispatch_map_[name] = std::move(handler);
}

std::vector<std::string> ToolExecutor::GetRegisteredToolNamesForTest() const {
  std::vector<std::string> names;
  names.reserve(dispatch_map_.size());
  for (const auto& [name, _] : dispatch_map_) {
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

absl::StatusOr<std::string> ToolExecutor::HandleQueryDb(const nlohmann::json& args) {
  if (!db_) {
    return absl::FailedPreconditionError("Database not initialized");
  }
  if (!args.is_object()) {
    return absl::InvalidArgumentError("Arguments must be a JSON object");
  }

  auto sql = json_get<std::string>(args, "sql");
  if (!sql) {
    return absl::InvalidArgumentError("'sql' must be a string");
  }

  std::vector<std::string> params;
  if (auto p_array = json_get<nlohmann::json::array_t>(args, "params")) {
    for (const auto& p : *p_array) {
      if (p.is_string()) {
        params.push_back(p.get<std::string>());
      } else if (p.is_null()) {
        params.emplace_back("NULL");
      } else {
        // For numbers, booleans, objects, and arrays, stringify them.
        params.push_back(p.dump());
      }
    }
  }
  return db_->Query(*sql, params);
}

absl::StatusOr<std::string> ToolExecutor::HandleReadFile(const nlohmann::json& args) {
  auto path = json_get<std::string>(args, "path");
  if (!path || path->empty()) {
    return absl::InvalidArgumentError("Missing mandatory field: path");
  }
  if (absl::StrContains(*path, "..") || (!path->empty() && (*path)[0] == '/')) {
    return absl::PermissionDeniedError("SECURITY_VIOLATION: Path traversal (..) or absolute paths are not allowed.");
  }

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
  if (start_line > end_line) {
    return absl::InvalidArgumentError(
        absl::StrCat("start_line (", start_line, ") cannot be greater than end_line (", end_line, ")"));
  }
  if (start_line > static_cast<int>(lines.size())) {
    return std::string();
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

absl::StatusOr<std::string> ToolExecutor::HandleListDirectory(const nlohmann::json& args) {
  const std::string path = json_get_or<std::string>(args, "path", ".");

  int raw_depth = 1;
  if (auto depth_int = json_get<int>(args, "depth")) {
    raw_depth = *depth_int;
  } else if (auto depth_str = json_get<std::string>(args, "depth")) {
    if (!absl::SimpleAtoi(*depth_str, &raw_depth)) {
      return absl::InvalidArgumentError("depth must be an integer");
    }
  }
  const int depth = std::max(1, std::min(8, raw_depth));

  const bool include_ignored = json_get_or<bool>(args, "include_ignored", false);
  std::vector<std::string> ignore_patterns;
  if (!include_ignored) {
    ignore_patterns = {".git", "node_modules", "bazel-*", "dist", "build", ".cache", ".next", "target"};
    if (args.contains("ignore") && !args["ignore"].is_null()) {
      if (!args["ignore"].is_array()) {
        return absl::InvalidArgumentError("ignore must be an array of strings");
      }
      for (const auto& it : args["ignore"]) {
        if (it.is_string() && !it.get<std::string>().empty()) {
          ignore_patterns.push_back(it.get<std::string>());
        }
      }
    }
  }

  std::string prune_clause;
  if (!ignore_patterns.empty()) {
    std::vector<std::string> names;
    names.reserve(ignore_patterns.size());
    for (const auto& p : ignore_patterns) {
      names.push_back(absl::StrCat("-name ", EscapeShellArg(p)));
    }
    prune_clause = absl::StrCat(" \\( ", absl::StrJoin(names, " -o "), " \\) -prune -o");
  }

  const std::string cmd = absl::StrCat(
      "cd ", EscapeShellArg(path), " && find . ",
      " -mindepth 1 -maxdepth ", depth,
      prune_clause,
      " -mindepth 1 -maxdepth ", depth,
      " -exec sh -c 'for f; do rel=\"${f#./}\"; if [ -d \"$f\" ]; then printf \"d\\t%s\\n\" \"$rel\"; else "
      "printf \"f\\t%s\\n\" \"$rel\"; fi; done' sh {} +");

  ASSIGN_OR_RETURN(auto run_res, RunCommand(cmd));
  if (run_res.exit_code != 0) {
    return absl::InternalError(absl::StrCat("Failed to list directory: ", run_res.stderr_out));
  }

  std::vector<std::string> out;
  std::stringstream ss(run_res.stdout_out);
  std::string row;
  while (std::getline(ss, row)) {
    if (row.empty()) continue;
    size_t tab = row.find('\t');
    if (tab == std::string::npos) continue;
    std::string type = row.substr(0, tab);
    std::string rel = row.substr(tab + 1);
    if (rel.empty()) continue;
    if (type == "d") {
      out.push_back(absl::StrCat("Directory: ", rel, "/"));
    } else {
      out.push_back(absl::StrCat("File: ", rel));
    }
  }
  return absl::StrJoin(out, "\n");
}

absl::StatusOr<std::string> ToolExecutor::HandleGrep(const nlohmann::json& args) {
  auto pattern = json_get<std::string>(args, "pattern");
  if (!pattern || pattern->empty()) {
    return absl::InvalidArgumentError("Missing mandatory field: pattern");
  }

  const std::string search_path = json_get_or<std::string>(args, "path", ".");

  ASSIGN_OR_RETURN(auto context_opt, ParseOptionalInteger(args, "context"));
  ASSIGN_OR_RETURN(auto limit_opt, ParseOptionalInteger(args, "limit"));
  const int context = std::max(0, context_opt.value_or(0));
  const int limit = std::max(1, limit_opt.value_or(200));

  std::vector<std::string> ignore_dirs;
  std::vector<std::string> ignore_files;
  const bool include_ignored = json_get_or<bool>(args, "include_ignored", false);
  if (!include_ignored) {
    ignore_dirs = {".git", "node_modules", "bazel-*", "dist", "build", ".cache", "target"};
  }

  if (const auto* ignore = json_at(args, "ignore")) {
    if (ignore->is_string() && !ignore->get<std::string>().empty()) {
      ignore_dirs.push_back(ignore->get<std::string>());
    } else if (ignore->is_array()) {
      for (const auto& v : *ignore) {
        if (v.is_string() && !v.get<std::string>().empty()) {
          ignore_dirs.push_back(v.get<std::string>());
        }
      }
    }
  }

  // Minimal root .gitignore support parity: plain dir/filename entries only.
  int skipped_gitignore_rules = 0;
  if (!include_ignored) {
    auto root_or = RunCommand("git rev-parse --show-toplevel 2>/dev/null");
    if (root_or.ok() && root_or->exit_code == 0) {
      std::string repo_root = TrimNewlines(root_or->stdout_out);
      if (!repo_root.empty()) {
        auto cat_or = RunCommand(absl::StrCat("cat ", EscapeShellArg(absl::StrCat(repo_root, "/.gitignore")), " 2>/dev/null"));
        if (cat_or.ok() && cat_or->exit_code == 0) {
          std::stringstream gs(cat_or->stdout_out);
          std::string line;
          while (std::getline(gs, line)) {
            std::string t(absl::StripAsciiWhitespace(line));
            if (t.empty() || t[0] == '#') continue;
            if (t[0] == '!' || absl::StrContains(t, "**")) {
              skipped_gitignore_rules++;
              continue;
            }
            if (!t.empty() && t.back() == '/') {
              t.pop_back();
              t = absl::StripAsciiWhitespace(t);
              if (t.empty() || absl::StrContains(t, "/")) {
                skipped_gitignore_rules++;
              } else {
                ignore_dirs.push_back(t);
              }
              continue;
            }
            if (absl::StrContains(t, "/")) {
              skipped_gitignore_rules++;
            } else {
              ignore_files.push_back(t);
            }
          }
        }
      }
    }
  }

  std::vector<std::string> exclude_args;
  exclude_args.reserve(ignore_dirs.size() + ignore_files.size());
  for (const auto& d : ignore_dirs) exclude_args.push_back(absl::StrCat("--exclude-dir=", EscapeShellArg(d)));
  for (const auto& f : ignore_files) exclude_args.push_back(absl::StrCat("--exclude=", EscapeShellArg(f)));

  const bool fixed_strings = json_get_or<bool>(args, "fixed_strings", false);
  const std::string mode_flag = fixed_strings ? "-F" : "-E";
  const std::string context_arg = context > 0 ? absl::StrCat(" -C ", context) : "";
  const std::string cmd = absl::StrCat("grep -rn ", mode_flag, " -I --color=never", context_arg,
                                       (exclude_args.empty() ? "" : absl::StrCat(" ", absl::StrJoin(exclude_args, " "))),
                                       " -e ", EscapeShellArg(*pattern), " ", EscapeShellArg(search_path));

  ASSIGN_OR_RETURN(auto res, RunCommand(cmd));
  if (res.exit_code != 0 && res.exit_code != 1) {
    return absl::InternalError(
        absl::StrCat("INTERNAL: Command failed with status ", res.exit_code, "\nOutput:\n", res.stdout_out, res.stderr_out));
  }

  std::string output = res.stdout_out;
  std::vector<std::string> lines;
  if (!output.empty()) {
    std::stringstream os(output);
    std::string l;
    while (std::getline(os, l)) lines.push_back(l);
  }
  if (static_cast<int>(lines.size()) > limit) {
    output = absl::StrCat(absl::StrJoin(std::vector<std::string>(lines.begin(), lines.begin() + limit), "\n"),
                          "\n[TRUNCATED: Use a more specific pattern or path to narrow results]");
  }
  if (skipped_gitignore_rules > 0) {
    if (!output.empty() && output.back() == '\n') output.pop_back();
    output = absl::StrCat(output, (output.empty() ? "" : "\n"), "[NOTE: Skipped ", skipped_gitignore_rules,
                          " unsupported root .gitignore rule(s)]");
  }
  return output;
}

absl::StatusOr<std::string> ToolExecutor::HandleGitCreateStagingBranch(const nlohmann::json& args) {
  auto name = json_get<std::string>(args, "name");
  if (!name || name->empty()) {
    return absl::InvalidArgumentError("name is required");
  }
  std::string normalized = *name;
  while (absl::StartsWith(normalized, "slop/staging/")) {
    normalized = normalized.substr(std::string("slop/staging/").size());
  }
  if (normalized.empty()) {
    return absl::InvalidArgumentError("name must contain non-prefix characters");
  }
  const std::string staging_name = absl::StrCat("slop/staging/", normalized);
  std::string base_branch;
  if (auto requested_base = json_get<std::string>(args, "base_branch")) {
    base_branch = *requested_base;
  } else {
    ASSIGN_OR_RETURN(base_branch, GetCurrentBranchName());
  }

  auto res_or = RunCommand(absl::StrCat("git checkout -b ", EscapeShellArg(staging_name), " ", EscapeShellArg(base_branch)));
  if (!res_or.ok()) return res_or.status();
  auto res = *res_or;
  if (res.exit_code != 0 &&
      (absl::StrContains(res.stdout_out, "already exists") || absl::StrContains(res.stderr_out, "already exists"))) {
    ASSIGN_OR_RETURN(res, RunCommand(absl::StrCat("git checkout ", EscapeShellArg(staging_name))));
  }
  if (res.exit_code != 0) {
    return absl::InternalError(
        absl::StrCat("Failed to create staging branch: ", res.stdout_out, res.stderr_out));
  }

  if (db_) {
    RETURN_IF_ERROR(db_->Query("INSERT OR REPLACE INTO staging_branches (branch_name, parent_branch) VALUES (?, ?)",
                               {staging_name, base_branch})
                        .status());
  }

  return absl::StrCat("Created and checked out staging branch: ", staging_name, " (base: ", base_branch, ")");
}

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

absl::StatusOr<std::string> ToolExecutor::HandleParseToolRows(const nlohmann::json& args) const {
  if (!args.is_object()) {
    return "[]";
  }

  nlohmann::json value = nlohmann::json();
  if (auto it = args.find("value"); it != args.end()) value = *it;

  auto parse_for_context = [](const nlohmann::json& v, const std::string& context)
      -> absl::StatusOr<nlohmann::json> {
    if (v.is_array()) return v;
    if (v.is_null()) return nlohmann::json::array();
    if (v.is_string()) {
      const std::string s = v.get<std::string>();
      if (s.empty()) return nlohmann::json::array();
      auto parsed = nlohmann::json::parse(s, nullptr, false);
      if (parsed.is_discarded()) {
        return absl::InvalidArgumentError(absl::StrCat("Failed to parse ", context, ": invalid JSON"));
      }
      if (parsed.is_array()) return parsed;
      return absl::InvalidArgumentError(absl::StrCat("Unexpected result shape for ", context));
    }
    if (v.is_object() && v.contains("rows") && v["rows"].is_array()) {
      return v["rows"];
    }
    return absl::InvalidArgumentError(absl::StrCat("Unexpected result shape for ", context));
  };

  ASSIGN_OR_RETURN(auto rows, parse_for_context(value, json_get_or<std::string>(args, "context", "context")));
  return rows.dump();
}

absl::StatusOr<std::string> ToolExecutor::HandleExecuteBash(const nlohmann::json& args) const {
  RETURN_IF_ERROR(MaybeEnforceMailStagingGuard(mail_mode_));

  auto command = json_get<std::string>(args, "command");
  if (!command) {
    return absl::InvalidArgumentError("Invalid arguments: command is required and must be a string");
  }

  const bool allow_nonzero_exit = json_get_or<bool>(args, "allow_nonzero_exit", false);
  std::string command_to_run = *command;
  if (auto cwd = json_get<std::string>(args, "cwd"); cwd && !cwd->empty()) {
    command_to_run = absl::StrCat("cd ", EscapeShellArg(*cwd), " && ", *command);
  }

  ASSIGN_OR_RETURN(auto run_res, RunCommand(command_to_run));
  const std::string stdout_text = run_res.stdout_out;
  const std::string stderr_text = run_res.stderr_out;
  std::string out_text = stdout_text;
  if (!stderr_text.empty()) {
    if (!out_text.empty() && out_text.back() != '\n') out_text.push_back('\n');
    absl::StrAppend(&out_text, "\n### STDERR\n", stderr_text);
  }

  if (run_res.exit_code != 0 && !allow_nonzero_exit) {
    std::string msg = absl::StrCat("INTERNAL: Command failed with status ", run_res.exit_code,
                                   "\nCommand:\n", command_to_run);
    if (!stdout_text.empty()) {
      absl::StrAppend(&msg, "\n\nStdout:\n", stdout_text);
    }
    if (!stderr_text.empty()) {
      absl::StrAppend(&msg, "\n\nStderr:\n", stderr_text);
    }
    return absl::StrCat("Error: ", msg);
  }

  // Preserve top-level behavior as printable output while still exposing
  // structured fields to run_js callers (via JSON parse fallback).
  const nlohmann::json payload = {
      {"stdout", stdout_text},
      {"stderr", stderr_text},
      {"exit_code", run_res.exit_code},
      {"exitCode", run_res.exit_code},
      {"output", out_text},
      {"command", *command},
      {"executed_command", command_to_run},
      {"toString", out_text},
  };
  return payload.dump();
}

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

absl::StatusOr<std::string> ToolExecutor::HandleGitCommitPatch(
    const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
  (void)cancellation;
  RETURN_IF_ERROR(MaybeEnforceMailStagingGuard(mail_mode_));

  auto summary = json_get<std::string>(args, "summary");
  const std::string rationale = json_get_or<std::string>(args, "rationale", "");
  if (!summary || summary->empty()) {
    return absl::InvalidArgumentError("Summary is required");
  }
  if (summary->size() > 50) {
    return absl::InvalidArgumentError("Summary must be <= 50 characters");
  }

  const std::string full_msg = absl::StrCat(*summary, "\n\n", rationale);
  ASSIGN_OR_RETURN(auto commit_res, RunCommand(absl::StrCat("git commit -m ", EscapeShellArg(full_msg))));
  if (commit_res.exit_code != 0) {
    return absl::InternalError(absl::StrCat("Commit failed: ", commit_res.stdout_out, commit_res.stderr_out));
  }

  return HandleGitFormatPatchSeries(nlohmann::json::object(), cancellation);
}

absl::StatusOr<std::string> ToolExecutor::HandleGitFormatPatchSeries(
    const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
  (void)cancellation;
  RETURN_IF_ERROR(MaybeEnforceMailStagingGuard(mail_mode_));
  ASSIGN_OR_RETURN(const std::string base_branch,
                   ResolveBaseBranch(db_, json_get_or<std::string>(args, "base_branch", "")));

  const std::string log_cmd =
      absl::StrCat("git log --reverse --format='### Patch [%n/%N]: %s ###%ncommit %H%nAuthor: %an <%ae>%nDate:   %ad%n%n    %s%n%n%b' ",
                   EscapeShellArg(base_branch), "..HEAD");
  ASSIGN_OR_RETURN(auto log_res, HandleExecuteBash({{"command", log_cmd}}));
  ASSIGN_OR_RETURN(auto diff_res,
                   HandleExecuteBash({{"command", absl::StrCat("git diff ", EscapeShellArg(base_branch), "..HEAD")}}));

  std::string log_output;
  std::string diff_output;
  auto parsed_log = nlohmann::json::parse(log_res, nullptr, false);
  auto parsed_diff = nlohmann::json::parse(diff_res, nullptr, false);
  if (parsed_log.is_object() && parsed_log.contains("output") && parsed_log["output"].is_string()) {
    log_output = parsed_log["output"].get<std::string>();
  }
  if (parsed_diff.is_object() && parsed_diff.contains("output") && parsed_diff["output"].is_string()) {
    diff_output = parsed_diff["output"].get<std::string>();
  }

  return absl::StrCat("--- MAIL SERIES ---\nBase: ", base_branch, "\n\n", log_output, "\n\n--- FULL DIFF ---\n", diff_output);
}

absl::StatusOr<std::string> ToolExecutor::HandleGitRerollPatch(
    const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
  RETURN_IF_ERROR(MaybeEnforceMailStagingGuard(mail_mode_));
  std::optional<int> idx;
  if (auto i = json_get<int>(args, "index")) {
    idx = *i;
  } else if (auto s = json_get<std::string>(args, "index")) {
    int parsed = 0;
    if (absl::SimpleAtoi(*s, &parsed)) idx = parsed;
  }
  ASSIGN_OR_RETURN(const std::string base_branch,
                   ResolveBaseBranch(db_, json_get_or<std::string>(args, "base_branch", "")));

  ASSIGN_OR_RETURN(auto log_res, HandleExecuteBash({{"command", absl::StrCat("git log --reverse --format=%H ", EscapeShellArg(base_branch), "..HEAD")}}));
  auto parsed_log = nlohmann::json::parse(log_res, nullptr, false);
  std::string hashes = parsed_log.is_object() && parsed_log.contains("stdout") && parsed_log["stdout"].is_string()
                           ? parsed_log["stdout"].get<std::string>()
                           : "";
  std::vector<std::string> commits;
  std::string token;
  for (char c : hashes) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!token.empty()) {
        commits.push_back(token);
        token.clear();
      }
    } else {
      token.push_back(c);
    }
  }
  if (!token.empty()) commits.push_back(token);

  const int index = idx.value_or(0);
  if (index < 1 || index > static_cast<int>(commits.size())) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid patch index ", index, " (total patches: ", commits.size(), ")"));
  }
  const std::string target_hash = commits[static_cast<size_t>(index - 1)];

  ASSIGN_OR_RETURN(auto fixup_res, RunCommand(absl::StrCat("git commit --fixup ", target_hash)));
  if (fixup_res.exit_code != 0) {
    return absl::InternalError("Failed to create fixup commit. Are there any changes staged?");
  }

  ASSIGN_OR_RETURN(auto rebase_res,
                   RunCommand(absl::StrCat("GIT_SEQUENCE_EDITOR=true git rebase -i --autosquash ", EscapeShellArg(base_branch))));
  if (rebase_res.exit_code != 0) {
    (void)RunCommand("git rebase --abort");
    return absl::InternalError(
        absl::StrCat("Rebase failed. You may have conflicts. Manual intervention required.\n", rebase_res.stderr_out));
  }

  ASSIGN_OR_RETURN(auto series, HandleGitFormatPatchSeries({{"base_branch", base_branch}}, cancellation));
  return absl::StrCat("Successfully rerolled patch ", index, "\n\n", series);
}

absl::StatusOr<std::string> ToolExecutor::HandleGitFinalizeSeries(
    const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
  (void)cancellation;
  RETURN_IF_ERROR(MaybeEnforceMailStagingGuard(mail_mode_));
  RETURN_IF_ERROR(AssertCleanWorkspace());
  if (!db_) return absl::FailedPreconditionError("Database not initialized");

  ASSIGN_OR_RETURN(const std::string current_branch, GetCurrentBranchName());
  ASSIGN_OR_RETURN(const std::string target_branch,
                   ResolveBaseBranch(db_, json_get_or<std::string>(args, "target_branch", "")));
  ASSIGN_OR_RETURN(auto hash_res, RunCommand("git rev-parse HEAD"));
  const std::string hash = std::string(absl::StripAsciiWhitespace(hash_res.stdout_out));

  ASSIGN_OR_RETURN(auto approval_rows_json,
                   db_->Query("SELECT branch_name, approved_hash FROM patch_approvals WHERE approved_hash = ?", {hash}));
  ASSIGN_OR_RETURN(auto approval_rows, ParseDbRows(approval_rows_json, "approval lookup"));
  const std::string canonical_current = CanonicalStagingBranch(current_branch);
  bool approved = false;
  for (const auto& row : approval_rows) {
    if (!row.is_object()) continue;
    const std::string row_hash = row.value("approved_hash", "");
    const std::string row_branch = row.value("branch_name", "");
    if (row_hash == hash && (row_branch == current_branch || CanonicalStagingBranch(row_branch) == canonical_current)) {
      approved = true;
      break;
    }
  }

  ASSIGN_OR_RETURN(auto landed_res,
                   RunCommand(absl::StrCat("git merge-base --is-ancestor ", EscapeShellArg(hash), " ",
                                           EscapeShellArg(target_branch))));
  const bool already_landed = landed_res.exit_code == 0;
  if (!approved && !already_landed) {
    return absl::FailedPreconditionError(
        absl::StrCat("Patch series not approved or hash mismatch. Please obtain approval for hash ", hash,
                     " before finalizing."));
  }

  ASSIGN_OR_RETURN(auto checkout_res, RunCommand(absl::StrCat("git checkout ", EscapeShellArg(target_branch))));
  if (checkout_res.exit_code != 0) {
    return absl::InternalError(
        absl::StrCat("Failed to checkout target branch '", target_branch, "': ", checkout_res.stderr_out));
  }

  if (!already_landed) {
    ASSIGN_OR_RETURN(auto merge_res, RunCommand(absl::StrCat("git merge --ff-only ", EscapeShellArg(current_branch))));
    if (merge_res.exit_code != 0) {
      (void)RunCommand(absl::StrCat("git checkout ", EscapeShellArg(current_branch)));
      return absl::InternalError(absl::StrCat("Merge failed: ", merge_res.stderr_out));
    }
  }

  bool deleted_staging_branch = false;
  if (current_branch != target_branch) {
    ASSIGN_OR_RETURN(auto del_res, RunCommand(absl::StrCat("git branch -D ", EscapeShellArg(current_branch))));
    deleted_staging_branch = del_res.exit_code == 0;
  }

  RETURN_IF_ERROR(db_->Query("DELETE FROM staging_branches WHERE branch_name = ?", {current_branch}).status());
  RETURN_IF_ERROR(db_->Query("DELETE FROM staging_branches WHERE branch_name = ?", {canonical_current}).status());
  RETURN_IF_ERROR(db_->Query("DELETE FROM patch_approvals WHERE branch_name = ?", {current_branch}).status());
  RETURN_IF_ERROR(db_->Query("DELETE FROM patch_approvals WHERE branch_name = ?", {canonical_current}).status());
  RETURN_IF_ERROR(db_->Query("UPDATE settings SET mode = 'standard' WHERE id = 1", {}).status());
  mail_mode_ = false;

  ASSIGN_OR_RETURN(auto final_head_res, RunCommand("git rev-parse HEAD"));
  const std::string final_head = std::string(absl::StripAsciiWhitespace(final_head_res.stdout_out));

  return nlohmann::json({{"ok", true},
                         {"action", "finalize_series"},
                         {"mail_mode", "off"},
                         {"previous_branch", current_branch},
                         {"current_branch", target_branch},
                         {"head", final_head},
                         {"approved", approved},
                         {"already_landed", already_landed},
                         {"merged", !already_landed},
                         {"deleted_staging_branch", deleted_staging_branch},
                         {"cleaned_metadata", true},
                         {"notes",
                          already_landed
                              ? nlohmann::json::array({"Patch already landed on target branch", "Cleaned staging metadata",
                                                       "Mail mode disabled"})
                              : nlohmann::json::array({"Series finalized and merged", "Cleaned staging metadata",
                                                       "Mail mode disabled"})}})
      .dump();
}

absl::StatusOr<std::string> ToolExecutor::HandleDescribeDb(const nlohmann::json& args) {
  (void)args;
  if (!db_) {
    return absl::FailedPreconditionError("Database not initialized");
  }
  // Keep output parity with JS implementation by returning raw query_db JSON.
  return db_->Query("SELECT name, sql FROM sqlite_master WHERE type='table'");
}

absl::StatusOr<std::string> ToolExecutor::HandleUseSkill(const nlohmann::json& args) {
  if (!db_) {
    return absl::FailedPreconditionError("Database not initialized");
  }
  if (!args.is_object()) {
    return absl::InvalidArgumentError("Arguments must be a JSON object");
  }

  auto name = json_get<std::string>(args, "name");
  if (!name || name->empty()) {
    return absl::InvalidArgumentError("Missing mandatory field: name");
  }

  std::string action = "activate";
  if (auto action_arg = json_get<std::string>(args, "action")) {
    action = *action_arg;
  }
  if (action != "activate" && action != "deactivate") {
    return absl::InvalidArgumentError("INVALID_ARGUMENT: action must be 'activate' or 'deactivate'");
  }

  if (session_id_.empty()) {
    return absl::FailedPreconditionError("No active session");
  }

  // Match historical JS behavior: fail when current session row does not exist.
  ASSIGN_OR_RETURN(auto session_rows_json, db_->Query("SELECT id, active_skills FROM sessions WHERE id = ?", {session_id_}));
  auto session_rows = nlohmann::json::parse(session_rows_json, nullptr, false);
  if (session_rows.is_discarded() || !session_rows.is_array()) {
    return absl::InternalError("Invalid session lookup response");
  }
  if (session_rows.empty()) {
    return absl::FailedPreconditionError(absl::StrCat("Session not found: ", session_id_));
  }

  auto exists_or = db_->SkillExists(*name);
  if (!exists_or.ok()) return exists_or.status();
  if (!*exists_or) {
    return absl::NotFoundError(absl::StrCat("UNKNOWN_SKILL: ", *name));
  }

  ASSIGN_OR_RETURN(auto active_skills, db_->GetActiveSkills(session_id_));

  std::string prompt_patch;
  if (action == "activate") {
    if (std::find(active_skills.begin(), active_skills.end(), *name) == active_skills.end()) {
      active_skills.push_back(*name);
      RETURN_IF_ERROR(db_->IncrementSkillActivationCount(*name));
    }

    ASSIGN_OR_RETURN(auto skills, db_->GetSkills());
    for (const auto& skill : skills) {
      if (skill.name == *name && !skill.system_prompt_patch.empty()) {
        prompt_patch = absl::StrCat("\n\n", skill.system_prompt_patch);
        break;
      }
    }
  } else {
    active_skills.erase(std::remove(active_skills.begin(), active_skills.end(), *name), active_skills.end());
  }

  RETURN_IF_ERROR(db_->SetActiveSkills(session_id_, active_skills));
  return absl::StrCat("Skill '", *name, "' ", (action == "activate" ? "activated" : "deactivated"), ".", prompt_patch);
}

void ToolExecutor::RegisterTools() {
  RegisterTool("query_db", [this](const nlohmann::json& args, auto) { return HandleQueryDb(args); });
  RegisterTool("read_file", [this](const nlohmann::json& args, auto) { return HandleReadFile(args); });
  RegisterTool("list_directory", [this](const nlohmann::json& args, auto) { return HandleListDirectory(args); });
  RegisterTool("describe_db", [this](const nlohmann::json& args, auto) { return HandleDescribeDb(args); });
  RegisterTool("grep", [this](const nlohmann::json& args, auto) { return HandleGrep(args); });
  RegisterTool("execute_bash", [this](const nlohmann::json& args, auto) { return HandleExecuteBash(args); });
  RegisterTool("patch_tool", [this](const nlohmann::json& args, auto) { return HandlePatchTool(args); });
  RegisterTool("write_file", [this](const nlohmann::json& args, auto) { return HandleWriteFile(args); });
  RegisterTool("parse_tool_rows", [this](const nlohmann::json& args, auto) { return HandleParseToolRows(args); });
  RegisterTool("use_skill", [this](const nlohmann::json& args, auto) { return HandleUseSkill(args); });
  RegisterTool("git_create_staging_branch",
               [this](const nlohmann::json& args, auto) { return HandleGitCreateStagingBranch(args); });
  RegisterTool("git_commit_patch", [this](const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
    return HandleGitCommitPatch(args, cancellation);
  });
  RegisterTool("git_format_patch_series",
               [this](const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
                 return HandleGitFormatPatchSeries(args, cancellation);
               });
  RegisterTool("git_reroll_patch", [this](const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
    return HandleGitRerollPatch(args, cancellation);
  });
  RegisterTool("git_finalize_series",
               [this](const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
                 return HandleGitFinalizeSeries(args, cancellation);
               });
  RegisterTool("grep_tool", [this](const nlohmann::json& args,
                                    std::shared_ptr<CancellationRequest> cancellation)
                                 -> absl::StatusOr<std::string> {
    if (!args.is_object() || !json_get<std::string>(args, "pattern")) {
      return absl::InvalidArgumentError("Missing mandatory field: pattern");
    }
    nlohmann::json simplified = {
        {"pattern", *json_get<std::string>(args, "pattern")},
    };
    if (auto path = json_get<std::string>(args, "path")) {
      simplified["path"] = *path;
    } else if (auto paths = json_get<std::string>(args, "paths")) {
      simplified["path"] = *paths;
    }
    if (auto context = json_get<int>(args, "context")) simplified["context"] = *context;
    if (auto limit = json_get<int>(args, "limit")) simplified["limit"] = *limit;
    if (auto include_ignored = json_get<bool>(args, "include_ignored")) simplified["include_ignored"] = *include_ignored;
    if (auto ignore = json_get<std::string>(args, "ignore")) simplified["ignore"] = *ignore;
    // Delegate to canonical grep implementation (currently JS-backed) to avoid behavior drift.
    return Execute("grep", simplified, cancellation);
  });

  RegisterTool("run_js", [this](const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
    return HandleRunJs(args, cancellation);
  });
  RegisterTool("ask_user", [this](const nlohmann::json& args, auto) -> absl::StatusOr<std::string> {
    std::string prompt_text = "Input required: ";
    if (auto p = json_get<std::string>(args, "prompt")) {
      prompt_text = *p;
    }

    while (true) {
      std::string response;
      if (ask_user_handler_) {
        response = ask_user_handler_(prompt_text);
      } else {
        std::cout << "\n" << ansi::Yellow << "Agent asks:\n" << ansi::Reset;
        slop::Renderer::Get().PrintMarkdown(prompt_text);
        response = slop::ReadLine("reply");
      }

      if (!absl::StartsWith(response, "/")) {
        return response;
      }

      std::cout << "\n"
                << ansi::Red << "Error: " << ansi::Reset
                << "/commands don't work in Q&A mode. Please provide a direct answer without using slash commands."
                << std::endl;
    }
  });

}

absl::StatusOr<std::string> ToolExecutor::Execute(const std::string& name, const nlohmann::json& args,
                                                  std::shared_ptr<CancellationRequest> cancellation) {
  if (IsDebugToolsEnabled()) {
    LOG(INFO) << "[tool_debug] Execute name=" << name << " args_keys=" << JsonKeys(args);
  }
  auto it = dispatch_map_.find(name);
  if (it != dispatch_map_.end()) {
    auto res = it->second(args, cancellation);
    if (IsDebugToolsEnabled()) {
      LOG(INFO) << "[tool_debug] Execute direct name=" << name << " status=" << (res.ok() ? "ok" : "error")
                << " output_preview=" << (res.ok() ? TruncateForLog(*res) : TruncateForLog(res.status().ToString()));
    }
    if (res.ok() && db_) {
      (void)db_->IncrementToolCallCount(name);
    }
    return res;
  }

  return absl::NotFoundError(absl::StrCat("NOT_FOUND: Tool not found: ", name));
}

void ToolExecutor::SetSessionId(const std::string& session_id) { session_id_ = session_id; }

void ToolExecutor::SetMailMode(bool enabled) {
  mail_mode_ = enabled;
  if (db_) {
    (void)db_->Query(enabled ? "UPDATE settings SET mode = 'mail' WHERE id = 1"
                             : "UPDATE settings SET mode = 'standard' WHERE id = 1");
  }
}

bool ToolExecutor::IsSkillActive(const std::string& name) {
  auto active = GetActiveSkills();
  return std::any_of(active.begin(), active.end(), [&name](const std::string& s) { return s == name; });
}

std::vector<std::string> ToolExecutor::GetActiveSkills() {
  if (session_id_.empty() || !db_) return {};
  auto skills_or = db_->GetActiveSkills(session_id_);
  if (skills_or.ok()) {
    return *skills_or;
  }
  return {};
}

absl::StatusOr<ToolExecutor::JsResult> ToolExecutor::RunJs(const RunJsRequest& req,
                                                           std::shared_ptr<CancellationRequest> cancellation) {
  if (IsDebugToolsEnabled()) {
    LOG(INFO) << "[tool_debug] RunJs begin script_len=" << req.script.size()
              << " script_preview=" << TruncateForLog(req.script) << " has_args=" << (!req.args.is_null());
    if (!req.args.is_null()) {
      LOG(INFO) << "[tool_debug] RunJs args_keys=" << JsonKeys(req.args);
    }
  }
  slop::JsInterpreter interpreter;
  std::stringstream stdout_buffer;
  interpreter.InitializeEnvironment(db_, dispatcher_.get(), cancellation, dispatch_map_, stdout_buffer);

  JSContext* ctx = interpreter.context();
  JSValue global_obj = JS_GetGlobalObject(ctx);

  JS_SetPropertyStr(ctx, global_obj, "session_id", JS_NewString(ctx, session_id_.c_str()));

  // Inject state
  auto state_res = db_->GetSessionState(session_id_);
  if (state_res.ok() && !state_res->empty()) {
    JS_SetPropertyStr(ctx, global_obj, "state", JS_NewString(ctx, state_res->c_str()));
  }

  if (!req.args.is_null()) {
    JS_SetPropertyStr(ctx, global_obj, "args", interpreter.JSONToJS(req.args));
  }
  JS_FreeValue(ctx, global_obj);

  // Load persistent functions from the database
  if (db_) {
    auto functions_res = db_->Query("SELECT name, code, json_schema FROM js_functions");
    if (functions_res.ok()) {
      if (auto functions_json = json_parse(*functions_res)) {
        if (auto rows = json_getter<std::vector<nlohmann::json>>::get(*functions_json)) {
          for (const auto& row : *rows) {
            auto name = json_get<std::string>(row, "name");
            auto code = json_get<std::string>(row, "code");
            auto json_schema = json_get<std::string>(row, "json_schema");
            if (name && code) {
              // Manifest-defined native tools may intentionally persist empty JS code.
              // Do not evaluate empty/whitespace code, otherwise we overwrite native
              // bindings with `undefined` through an empty IIFE.
              std::string code_text = *code;
              auto first_non_ws = code_text.find_first_not_of(" \t\n\r");
              if (first_non_ws == std::string::npos) {
                continue;
              }

              std::string target = (json_schema && !json_schema->empty()) ? "tools" : "globalThis";
              std::string wrapped_code = target + "['" + *name + "'] = (function() {\n" + code_text + "\n})();";
              JSValue func_res = interpreter.RunString(wrapped_code, "js_function_" + *name + ".js", false);
              JS_FreeValue(ctx, func_res);
            }
          }
        }
      }
    }
  }

  // Load preamble
  JSValue preamble_res = interpreter.RunString(slop::kJsPreamble, "preamble.js", false);
  JS_FreeValue(ctx, preamble_res);

  JSValue result = interpreter.RunString(req.script, "input.js");

  JsResult res;
  res.stdout_out = stdout_buffer.str();
  bool had_js_return_value = false;
  if (JS_IsException(result)) {
    JSValue exception = JS_GetException(ctx);
    absl::Status status = absl::InternalError(absl::StrCat("JS Error\nOutput:\n", res.stdout_out));
    if (const char* str = JS_ToCString(ctx, exception)) {
      status = absl::InternalError(absl::StrCat(str, "\nOutput:\n", res.stdout_out));
      JS_FreeCString(ctx, str);
    }
    JS_FreeValue(ctx, exception);
    JS_FreeValue(ctx, result);
    if (IsDebugToolsEnabled()) {
      LOG(INFO) << "[tool_debug] RunJs exception status=" << status;
    }
    return status;
  }

  if (!JS_IsUndefined(result)) {
    had_js_return_value = true;
    JSValue printable = result;
    bool owns_printable = false;
    if (JS_IsObject(result)) {
      JSValue json_value = JS_JSONStringify(ctx, result, JS_UNDEFINED, JS_UNDEFINED);
      if (!JS_IsException(json_value)) {
        printable = json_value;
        owns_printable = true;
      } else {
        // Clear stringify exception and fall back to default JS string coercion.
        JSValue exception = JS_GetException(ctx);
        JS_FreeValue(ctx, exception);
      }
    }
    const char* str = JS_ToCString(ctx, printable);
    if (str) {
      res.return_value = str;
      JS_FreeCString(ctx, str);
    }
    if (owns_printable) {
      JS_FreeValue(ctx, printable);
    }
  }
  JS_FreeValue(ctx, result);
  res.has_js_return_value = had_js_return_value;
  if (IsDebugToolsEnabled()) {
    LOG(INFO) << "[tool_debug] RunJs success stdout_len=" << res.stdout_out.size()
              << " return_len=" << res.return_value.size() << " return_preview=" << TruncateForLog(res.return_value);
    if (!had_js_return_value) {
      LOG(INFO) << "[tool_debug] RunJs JS result was undefined. Script likely executed without an explicit return.";
    }
  }
  return res;
}

absl::StatusOr<std::string> ToolExecutor::HandleRunJs(const nlohmann::json& args,
                                                      std::shared_ptr<CancellationRequest> cancellation) {
  if (IsDebugToolsEnabled()) {
    LOG(INFO) << "[tool_debug] HandleRunJs args_keys=" << JsonKeys(args);
  }
  RunJsRequest req;
  auto script = json_get<std::string>(args, "script");
  const auto* nested_args = json_at(args, "args");
  if (!script && nested_args != nullptr && nested_args->is_object()) {
    script = json_get<std::string>(*nested_args, "script");
  }
  if (!script) {
    script = json_get<std::string>(args, "code");
  }
  if (!script) {
    script = json_get<std::string>(args, "javascript");
  }
  if (!script) {
    std::string arg_shape = args.is_object() ? json_dump(args) : std::string("<non-object>");
    if (arg_shape.size() > 512) {
      arg_shape = arg_shape.substr(0, 512) + "...";
    }
    return absl::InvalidArgumentError(absl::StrCat(
        "'script' must be a string (also accepted: args.script, code, javascript). Received: ", arg_shape));
  }
  req.script = *script;
  if (nested_args != nullptr) {
    req.args = *nested_args;
  }
  if (IsDebugToolsEnabled()) {
    LOG(INFO) << "[tool_debug] HandleRunJs resolved script_source="
              << (json_get<std::string>(args, "script")
                      ? "script"
                      : (nested_args != nullptr && json_get<std::string>(*nested_args, "script")
                             ? "args.script"
                             : (json_get<std::string>(args, "code") ? "code" : "javascript")))
              << " script_len=" << req.script.size();
  }
  auto res = RunJs(req, cancellation);
  if (!res.ok()) return res.status();
  std::string output = res->FullOutput();
  if (output.empty()) {
    return absl::FailedPreconditionError("run_js produced no output: script must return a value or print output");
  }
  return output;
}

absl::StatusOr<std::string> ToolExecutor::GetBaseBranch(const std::string& requested_base) {
  RunJsRequest req;
  req.script = "return git.get_base_branch(args.requested_base)";
  req.args["requested_base"] = requested_base;
  auto res = RunJs(req, nullptr);
  if (!res.ok()) return res.status();
  return res->return_value;
}

}  // namespace slop



















































