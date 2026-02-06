#include "core/tool_executor.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/substitute.h"

#include "core/shell_util.h"
namespace slop {

absl::StatusOr<std::string> ToolExecutor::Execute(const std::string& name, const nlohmann::json& args,
                                                  std::shared_ptr<CancellationRequest> cancellation) {
  LOG(INFO) << "Executing tool: " << name
            << " with args: " << args.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  auto wrap_result = [&](const std::string& tool_name, const std::string& content) {
    return absl::StrCat("### TOOL_RESULT: ", tool_name, "\n", content, "\n\n---");
  };

  absl::StatusOr<std::string> result;
  if (name == "read_file") {
    result = ReadFile(args.get<ReadFileRequest>());
  } else if (name == "write_file") {
    result = WriteFile(args.get<WriteFileRequest>());
  } else if (name == "apply_patch") {
    result = ApplyPatch(args.get<ApplyPatchRequest>());
  } else if (name == "grep_tool") {
    auto req = args.get<GrepRequest>();
    // Delegate to GitGrep if in a git repo
    ExecuteBashRequest git_check_req;
    git_check_req.command = "git rev-parse --is-inside-work-tree";
    auto git_repo_check = ExecuteBash(git_check_req, cancellation);
    if (git_repo_check.ok() && git_repo_check->find("true") != std::string::npos) {
      GitGrepRequest git_req;
      git_req.pattern = req.pattern;
      git_req.path = {req.path};
      git_req.context = req.context;
      auto git_res = GitGrep(git_req, cancellation);
      if (git_res.ok() && !git_res->empty() && git_res->find("Error:") == std::string::npos) {
        result = git_res;
      } else {
        result = Grep(req, cancellation);
      }
    } else {
      auto grep_res = Grep(req, cancellation);
      if (grep_res.ok()) {
        result =
            "Notice: Not a git repository. Consider running 'git init' for better search performance and feature "
            "support.\n\n" +
            *grep_res;
      } else {
        result = grep_res;
      }
    }
  } else if (name == "git_grep_tool") {
    result = GitGrep(args.get<GitGrepRequest>(), cancellation);
  } else if (name == "execute_bash") {
    result = ExecuteBash(args.get<ExecuteBashRequest>(), cancellation);
  } else if (name == "query_db") {
    result = QueryDb(args.get<QueryDbRequest>());
  } else if (name == "save_memo") {
    result = SaveMemo(args.get<SaveMemoRequest>());
  } else if (name == "retrieve_memos") {
    result = RetrieveMemos(args.get<RetrieveMemosRequest>());
  } else if (name == "list_directory") {
    result = ListDirectory(args.get<ListDirectoryRequest>(), cancellation);
  } else if (name == "manage_scratchpad") {
    result = ManageScratchpad(args.get<ManageScratchpadRequest>());
  } else if (name == "describe_db") {
    result = DescribeDb();
  } else if (name == "use_skill") {
    result = UseSkill(args.get<UseSkillRequest>());
  } else if (name == "search_code") {
    result = SearchCode(args.get<SearchCodeRequest>(), cancellation);
  } else {
    return absl::NotFoundError("Tool not found: " + name);
  }

  if (!result.ok()) {
    std::string error_msg = result.status().ToString();
    // Truncate long error messages for logging
    std::string log_msg = error_msg;
    if (size_t first_nl = log_msg.find('\n'); first_nl != std::string::npos) {
      log_msg = log_msg.substr(0, first_nl) + " (multi-line)...";
    }
    if (log_msg.length() > 100) {
      log_msg = log_msg.substr(0, 97) + "...";
    }
    LOG(WARNING) << "Tool " << name << " failed: " << log_msg;
    return wrap_result(name, "Error: " + error_msg);
  }
  LOG(INFO) << "Tool " << name << " succeeded (" << result->size() << " bytes).";
  (void)db_->IncrementToolCallCount(name);
  return wrap_result(name, *result);
}

absl::StatusOr<std::string> ToolExecutor::ReadFile(const ReadFileRequest& req) {
  if (req.start_line && req.end_line && *req.start_line > *req.end_line) {
    return absl::InvalidArgumentError("start_line must be less than or equal to end_line");
  }

  std::ifstream file(req.path);
  if (!file.is_open()) return absl::NotFoundError("Could not open file: " + req.path);

  std::stringstream ss;
  std::string line;
  int current_line = 1;
  int total_lines = 0;
  {
    std::string dummy;
    while (std::getline(file, dummy)) total_lines++;
    file.clear();  // Clear EOF bit
    file.seekg(0, std::ios::beg);
  }

  while (std::getline(file, line)) {
    if ((!req.start_line || current_line >= *req.start_line) && (!req.end_line || current_line <= *req.end_line)) {
      if (req.add_line_numbers) {
        ss << current_line << ": " << line << "\n";
      } else {
        ss << line << "\n";
      }
    }
    current_line++;
    if (req.end_line && current_line > *req.end_line) {
      break;
    }
  }
  std::string result = ss.str();

  int s = req.start_line.value_or(1);
  int e = req.end_line.value_or(total_lines);
  std::string header = absl::Substitute("### FILE: $0 | TOTAL_LINES: $1 | RANGE: $2-$3\n", req.path, total_lines, s, e);

  if (e < total_lines) {
    absl::StrAppend(&result, "\n... [Truncated. Use 'read_file' with start_line=", e + 1, " to see more] ...");
  }

  return header + result;
}

absl::StatusOr<std::string> ToolExecutor::WriteFile(const WriteFileRequest& req) {
  std::ofstream file(req.path);
  if (!file.is_open()) return absl::InternalError("Could not open file for writing: " + req.path);
  file << req.content;
  file.close();

  // Get the size of the content written
  size_t bytes_written = req.content.size();

  // Create a preview of the content (first 3 lines or less)
  std::stringstream preview;
  std::stringstream content_stream(req.content);
  std::string line;
  int line_count = 0;
  while (std::getline(content_stream, line) && line_count < 3) {
    preview << line << "\n";
    line_count++;
  }

  // Return a more detailed result
  std::string result = "File written successfully:\n";
  result += "Path: " + req.path + "\n";
  result += "Bytes written: " + std::to_string(bytes_written) + "\n";
  result += "Preview:\n" + preview.str();

  return result;
}

absl::StatusOr<std::string> ToolExecutor::ApplyPatch(const ApplyPatchRequest& req) {
  std::ifstream ifs(req.path, std::ios::in | std::ios::binary | std::ios::ate);
  if (!ifs.is_open()) return absl::NotFoundError("Could not open file: " + req.path);
  std::ifstream::pos_type fileSize = ifs.tellg();
  ifs.seekg(0, std::ios::beg);
  std::string content(static_cast<size_t>(fileSize), '\0');
  ifs.read(content.data(), fileSize);

  for (const auto& patch : req.patches) {
    if (patch.find.empty()) return absl::InvalidArgumentError("Patch 'find' string cannot be empty");

    size_t pos = content.find(patch.find);
    if (pos == std::string::npos) {
      return absl::NotFoundError(absl::StrCat("Could not find exact match for: ", patch.find));
    }
    if (content.find(patch.find, pos + 1) != std::string::npos) {
      return absl::FailedPreconditionError(absl::StrCat("Ambiguous match for: ", patch.find));
    }

    content.replace(pos, patch.find.length(), patch.replace);
  }

  return WriteFile({req.path, content});
}

absl::StatusOr<std::string> ToolExecutor::QueryDb(const QueryDbRequest& req) { return db_->Query(req.sql); }

absl::StatusOr<std::string> ToolExecutor::ExecuteBash(const ExecuteBashRequest& req,
                                                      std::shared_ptr<CancellationRequest> cancellation) {
  auto res = RunCommand(req.command, cancellation);
  if (!res.ok()) return res.status();
  std::string output = res->stdout_out;
  if (!res->stderr_out.empty()) {
    if (!output.empty() && output.back() != '\n') output += "\n";
    output += "### STDERR\n" + res->stderr_out;
  }
  if (res->exit_code != 0) {
    return absl::InternalError(absl::StrCat("Command failed with status ", res->exit_code, ": ", output));
  }
  return output;
}

absl::StatusOr<std::string> ToolExecutor::Grep(const GrepRequest& req,
                                               std::shared_ptr<CancellationRequest> cancellation) {
  std::string cmd = "grep -n";
  if (std::filesystem::is_directory(req.path)) {
    cmd += "r";
  }
  if (req.context > 0) {
    cmd += " -C " + std::to_string(req.context);
  }
  cmd += " -e " + EscapeShellArg(req.pattern) + " " + EscapeShellArg(req.path);

  auto res = RunCommand(cmd, cancellation);
  if (!res.ok()) return res.status();
  if (res->exit_code != 0 && res->exit_code != 1) {
    std::string err = res->stdout_out;
    if (!res->stderr_out.empty()) {
      if (!err.empty() && err.back() != '\n') err += "\n";
      err += "### STDERR\n" + res->stderr_out;
    }
    return absl::InternalError(absl::StrCat("Command failed with status ", res->exit_code, ": ", err));
  }

  std::stringstream ss(res->stdout_out);
  std::string line;
  std::string output;
  int count = 0;
  while (std::getline(ss, line) && count < 50) {
    output += line + "\n";
    count++;
  }
  if (std::getline(ss, line)) {
    output += "\n[TRUNCATED: Use a more specific pattern or path to narrow results]\n";
  }
  return output;
}

absl::StatusOr<std::string> ToolExecutor::SearchCode(const SearchCodeRequest& req,
                                                     std::shared_ptr<CancellationRequest> cancellation) {
  GrepRequest grep_req;
  grep_req.pattern = req.query;
  grep_req.path = ".";
  grep_req.context = 0;
  return Grep(grep_req, cancellation);
}

absl::StatusOr<std::string> ToolExecutor::GitGrep(const GitGrepRequest& req,
                                                  std::shared_ptr<CancellationRequest> cancellation) {
  // Check if git is available
  ExecuteBashRequest git_check_req;
  git_check_req.command = "git --version";
  auto git_check = ExecuteBash(git_check_req, cancellation);
  if (!git_check.ok() || git_check->find("git version") == std::string::npos) {
    return "Error: git is not available on this system. git_grep_tool is not supported.";
  }

  // Check if it is a git repository
  ExecuteBashRequest git_repo_req;
  git_repo_req.command = "git rev-parse --is-inside-work-tree";
  auto git_repo_check = ExecuteBash(git_repo_req, cancellation);
  if (!git_repo_check.ok() || git_repo_check->find("true") == std::string::npos) {
    return "Error: not a git repository. git_grep_tool is not supported.";
  }

  std::string cmd = "git grep";

  if (req.line_number) cmd += " -n";
  if (req.case_insensitive) cmd += " -i";
  if (req.count) cmd += " -c";
  if (req.show_function) cmd += " -p";
  if (req.function_context) cmd += " -W";
  if (req.files_with_matches) cmd += " -l";
  if (req.word_regexp) cmd += " -w";
  if (req.pcre) cmd += " -P";
  if (req.cached) cmd += " --cached";
  if (req.all_match) cmd += " --all-match";

  if (req.context) {
    cmd += " -C " + std::to_string(*req.context);
  } else {
    if (req.before) cmd += " -B " + std::to_string(*req.before);
    if (req.after) cmd += " -A " + std::to_string(*req.after);
  }

  if (req.branch) {
    cmd += " " + EscapeShellArg(*req.branch);
  }

  if (!req.patterns.empty()) {
    for (const auto& p : req.patterns) {
      if (p == "--and" || p == "--or" || p == "--not" || p == "(" || p == ")") {
        cmd += " " + EscapeShellArg(p);
      } else {
        cmd += " -e " + EscapeShellArg(p);
      }
    }
  } else if (req.pattern) {
    cmd += " -e " + EscapeShellArg(*req.pattern);
  }

  if (req.untracked) cmd += " --untracked";
  if (req.no_index) cmd += " --no-index";
  if ((req.untracked || req.no_index) && req.exclude_standard) {
    cmd += " --exclude-standard";
  }
  if (req.fixed_strings) cmd += " -F";

  if (req.max_depth) {
    cmd += " --max-depth " + std::to_string(*req.max_depth);
  }

  if (!req.path.empty()) {
    cmd += " --";
    for (const auto& p : req.path) {
      cmd += " " + EscapeShellArg(p);
    }
  }

  auto res = RunCommand(cmd, cancellation);
  if (!res.ok()) return res.status();
  if (res->exit_code != 0 && res->exit_code != 1) {
    std::string err = res->stdout_out;
    if (!res->stderr_out.empty()) {
      if (!err.empty() && err.back() != '\n') err += "\n";
      err += "### STDERR\n" + res->stderr_out;
    }
    return absl::InternalError(absl::StrCat("Command failed with status ", res->exit_code, ": ", err));
  }

  std::stringstream ss(res->stdout_out);
  std::string line;
  std::string output;
  int count = 0;
  while (std::getline(ss, line) && count < 500) {
    output += line + "\n";
    count++;
  }
  if (std::getline(ss, line)) {
    output += "\n[TRUNCATED: Use a more specific pattern or path to narrow results]\n";
  }

  size_t line_count = count;
  // If the result is substantial, prepend a summary of matches per file.
  if (line_count > 20 && !absl::StrContains(cmd, " -c") && !absl::StrContains(cmd, " -l") &&
      !absl::StrContains(cmd, " -L")) {
    std::string count_cmd = cmd + " -c";
    auto count_res = RunCommand(count_cmd);
    if (count_res.ok() && count_res->exit_code == 0) {
      output = absl::StrCat("### SEARCH_SUMMARY:\n", count_res->stdout_out, "---\n", output);
    }
  }

  return output;
}

absl::StatusOr<std::string> ToolExecutor::SaveMemo(const SaveMemoRequest& req) {
  nlohmann::json tags_json = req.tags;
  auto status = db_->AddMemo(req.content, tags_json.dump());
  if (!status.ok()) return status;
  return "Memo saved successfully.";
}

absl::StatusOr<std::string> ToolExecutor::RetrieveMemos(const RetrieveMemosRequest& req) {
  auto memos_or = db_->GetMemosByTags(req.tags);
  if (!memos_or.ok()) return memos_or.status();

  nlohmann::json result = nlohmann::json::array();
  for (const auto& m : *memos_or) {
    result.push_back({
        {"id", m.id},
        {"content", m.content},
        {"tags", nlohmann::json::parse(m.semantic_tags)},
        {"created_at", m.created_at},
    });
  }
  return result.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

absl::StatusOr<std::string> ToolExecutor::ListDirectory(const ListDirectoryRequest& req,
                                                        std::shared_ptr<CancellationRequest> cancellation) {
  int max_depth = req.depth.value_or(1);

  ExecuteBashRequest git_check_req;
  git_check_req.command = "git rev-parse --is-inside-work-tree";
  auto git_repo_check = ExecuteBash(git_check_req, cancellation);
  if (req.git_only && git_repo_check.ok() && git_repo_check->find("true") != std::string::npos) {
    std::string cmd = "git ls-files --cached --others --exclude-standard";
    if (req.path != ".") {
      cmd += " " + req.path;
    }
    ExecuteBashRequest git_ls_req;
    git_ls_req.command = cmd;
    auto git_res = ExecuteBash(git_ls_req, cancellation);
    if (git_res.ok()) {
      return git_res;
    }
  }

  // Fallback to std::filesystem
  std::stringstream ss;
  if (!std::filesystem::exists(req.path)) return absl::NotFoundError("Directory not found: " + req.path);

  for (const auto& entry : std::filesystem::recursive_directory_iterator(req.path)) {
    auto relative = std::filesystem::relative(entry.path(), req.path);
    int depth = std::distance(relative.begin(), relative.end());
    if (depth > max_depth) continue;

    if (entry.is_directory()) {
      ss << "Directory: " << relative.string() << "/\n";
    } else {
      ss << "File: " << relative.string() << "\n";
    }
  }

  return ss.str();
}

absl::StatusOr<std::string> ToolExecutor::ManageScratchpad(const ManageScratchpadRequest& req) {
  if (session_id_.empty()) return absl::FailedPreconditionError("No active session");

  if (req.action == "read") {
    auto res = db_->GetScratchpad(session_id_);
    if (!res.ok()) {
      if (absl::IsNotFound(res.status())) return "Scratchpad is empty.";
      return res.status();
    }
    if (res->empty()) return "Scratchpad is empty.";
    return *res;
  }
  if (req.action == "update") {
    if (!req.content) return absl::InvalidArgumentError("Missing 'content' for update");
    auto status = db_->UpdateScratchpad(session_id_, *req.content);
    if (!status.ok()) return status;
    return "Scratchpad updated.";
  }
  if (req.action == "append") {
    if (!req.content) return absl::InvalidArgumentError("Missing 'content' for append");
    auto current = db_->GetScratchpad(session_id_);
    std::string new_content = (current.ok() ? *current : "") + *req.content;
    auto status = db_->UpdateScratchpad(session_id_, new_content);
    if (!status.ok()) return status;
    return "Content appended to scratchpad.";
  }
  return absl::InvalidArgumentError("Unknown action: " + req.action);
}

absl::StatusOr<std::string> ToolExecutor::DescribeDb() {
  return db_->Query("SELECT name, sql FROM sqlite_master WHERE type='table'");
}

absl::StatusOr<std::string> ToolExecutor::UseSkill(const UseSkillRequest& req) {
  if (session_id_.empty()) return absl::FailedPreconditionError("No active session");

  auto active_skills_or = db_->GetActiveSkills(session_id_);
  if (!active_skills_or.ok()) return active_skills_or.status();
  std::vector<std::string> active_skills = *active_skills_or;

  if (req.action == "activate") {
    // Increment count
    auto status = db_->IncrementSkillActivationCount(req.name);
    if (!status.ok()) return status;

    // Add to active if not present
    if (std::find(active_skills.begin(), active_skills.end(), req.name) == active_skills.end()) {
      active_skills.push_back(req.name);
      status = db_->SetActiveSkills(session_id_, active_skills);
      if (!status.ok()) return status;
    }

    // Return patch
    auto skills_or = db_->GetSkills();
    if (!skills_or.ok()) return skills_or.status();
    for (const auto& s : *skills_or) {
      if (s.name == req.name) {
        return "Skill '" + req.name + "' activated.\n\n" + s.system_prompt_patch;
      }
    }
    return absl::NotFoundError("Skill not found: " + req.name);
  }

  if (req.action == "deactivate") {
    auto it = std::find(active_skills.begin(), active_skills.end(), req.name);
    if (it != active_skills.end()) {
      active_skills.erase(it);
      auto status = db_->SetActiveSkills(session_id_, active_skills);
      if (!status.ok()) return status;
      return "Skill '" + req.name + "' deactivated.";
    }
    return "Skill '" + req.name + "' was not active.";
  }

  return absl::InvalidArgumentError("Unknown action: " + req.action);
}

}  // namespace slop
