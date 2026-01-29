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

#include "core/shell_util.h"
namespace slop {

absl::StatusOr<std::string> ToolExecutor::Execute(const std::string& name, const nlohmann::json& args) {
  LOG(INFO) << "Executing tool: " << name << " with args: " << args.dump();
  auto wrap_result = [&](const std::string& tool_name, const std::string& content) {
    return absl::StrCat("### TOOL_RESULT: ", tool_name, "\n", content, "\n\n---");
  };

  absl::StatusOr<std::string> result;
  if (name == "read_file") {
    if (!args.contains("path")) return absl::InvalidArgumentError("Missing 'path' argument");
    std::optional<int> start_line;
    if (args.contains("start_line")) start_line = args["start_line"].get<int>();
    std::optional<int> end_line;
    if (args.contains("end_line")) end_line = args["end_line"].get<int>();
    result = ReadFile(args["path"], start_line, end_line, true);
  } else if (name == "write_file") {
    if (!args.contains("path")) return absl::InvalidArgumentError("Missing 'path' argument");
    if (!args.contains("content")) return absl::InvalidArgumentError("Missing 'content' argument");
    result = WriteFile(args["path"], args["content"]);
  } else if (name == "apply_patch") {
    if (!args.contains("path")) return absl::InvalidArgumentError("Missing 'path' argument");
    if (!args.contains("patches")) return absl::InvalidArgumentError("Missing 'patches' argument");
    result = ApplyPatch(args["path"], args["patches"]);
  } else if (name == "grep_tool") {
    if (!args.contains("pattern")) return absl::InvalidArgumentError("Missing 'pattern' argument");

    std::string path = args.contains("path") ? args["path"].get<std::string>() : ".";
    int context = args.contains("context") ? args["context"].get<int>() : 0;

    // Delegate to GitGrep if in a git repo
    auto git_repo_check = ExecuteBash("git rev-parse --is-inside-work-tree");
    if (git_repo_check.ok() && git_repo_check->find("true") != std::string::npos) {
      auto git_res = GitGrep(args);
      if (git_res.ok() && !git_res->empty() && git_res->find("Error:") == std::string::npos) {
        result = git_res;
      } else {
        result = Grep(args["pattern"], path, context);
      }
    } else {
      auto grep_res = Grep(args["pattern"], path, context);
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
    result = GitGrep(args);
  } else if (name == "execute_bash") {
    if (!args.contains("command")) return absl::InvalidArgumentError("Missing 'command' argument");
    result = ExecuteBash(args["command"]);
  } else if (name == "query_db") {
    if (!args.contains("sql")) return absl::InvalidArgumentError("Missing 'sql' argument");
    result = db_->Query(args["sql"]);
  } else if (name == "save_memo") {
    if (!args.contains("content")) return absl::InvalidArgumentError("Missing 'content' argument");
    if (!args.contains("tags")) return absl::InvalidArgumentError("Missing 'tags' argument");
    result = SaveMemo(args["content"], args["tags"].get<std::vector<std::string>>());
  } else if (name == "retrieve_memos") {
    if (!args.contains("tags")) return absl::InvalidArgumentError("Missing 'tags' argument");
    result = RetrieveMemos(args["tags"].get<std::vector<std::string>>());
  } else if (name == "list_directory") {
    result = ListDirectory(args);
  } else if (name == "manage_scratchpad") {
    result = ManageScratchpad(args);
  } else if (name == "describe_db") {
    result = DescribeDb();
  } else if (name == "use_skill") {
    result = UseSkill(args);
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
  LOG(INFO) << "Tool " << name << " succeeded.";
  return wrap_result(name, *result);
}

absl::StatusOr<std::string> ToolExecutor::ReadFile(const std::string& path, std::optional<int> start_line,
                                                   std::optional<int> end_line, bool add_line_numbers) {
  if (start_line && end_line && *start_line > *end_line) {
    return absl::InvalidArgumentError("start_line must be less than or equal to end_line");
  }

  std::ifstream file(path);
  if (!file.is_open()) return absl::NotFoundError("Could not open file: " + path);

  std::stringstream ss;
  std::string line;
  int current_line = 1;
  while (std::getline(file, line)) {
    if ((!start_line || current_line >= *start_line) && (!end_line || current_line <= *end_line)) {
      if (add_line_numbers) {
        ss << current_line << ": " << line << "\n";
      } else {
        ss << line << "\n";
      }
    }
    current_line++;
    if (end_line && current_line > *end_line) break;
  }

  std::string result = ss.str();
  if (!start_line && !end_line && current_line > 100) {
    result = "[NOTICE: This is a large file (" + std::to_string(current_line - 1) +
             " lines). Consider using line ranges in future calls to preserve context space]\n" + result;
  }
  return result;
}

absl::StatusOr<std::string> ToolExecutor::WriteFile(const std::string& path, const std::string& content) {
  std::ofstream file(path);
  if (!file.is_open()) return absl::InternalError("Could not open file for writing: " + path);
  file << content;
  file.close();

  // Get the size of the content written
  size_t bytes_written = content.size();

  // Create a preview of the content (first 3 lines or less)
  std::stringstream preview;
  std::stringstream content_stream(content);
  std::string line;
  int line_count = 0;
  while (std::getline(content_stream, line) && line_count < 3) {
    preview << line << "\n";
    line_count++;
  }

  // Return a more detailed result
  std::string result = "File written successfully:\n";
  result += "Path: " + path + "\n";
  result += "Bytes written: " + std::to_string(bytes_written) + "\n";
  result += "Preview:\n" + preview.str();

  return result;
}

absl::StatusOr<std::string> ToolExecutor::ApplyPatch(const std::string& path, const nlohmann::json& patches) {
  std::ifstream ifs(path, std::ios::in | std::ios::binary | std::ios::ate);
  if (!ifs.is_open()) return absl::NotFoundError("Could not open file: " + path);
  std::ifstream::pos_type fileSize = ifs.tellg();
  ifs.seekg(0, std::ios::beg);
  std::string content(static_cast<size_t>(fileSize), '\0');
  ifs.read(content.data(), fileSize);

  if (!patches.is_array()) return absl::InvalidArgumentError("'patches' must be an array");

  for (const auto& patch : patches) {
    if (!patch.contains("find") || !patch.contains("replace")) {
      return absl::InvalidArgumentError("Each patch must contain 'find' and 'replace'");
    }
    std::string find_str = patch["find"];
    std::string replace_str = patch["replace"];

    if (find_str.empty()) return absl::InvalidArgumentError("Patch 'find' string cannot be empty");

    size_t pos = content.find(find_str);
    if (pos == std::string::npos) {
      return absl::NotFoundError(absl::StrCat("Could not find exact match for: ", find_str));
    }
    if (content.find(find_str, pos + 1) != std::string::npos) {
      return absl::FailedPreconditionError(absl::StrCat("Ambiguous match for: ", find_str));
    }

    content.replace(pos, find_str.length(), replace_str);
  }

  return WriteFile(path, content);
}

absl::StatusOr<std::string> ToolExecutor::ExecuteBash(const std::string& command) {
  auto res = RunCommand(command);
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

absl::StatusOr<std::string> ToolExecutor::Grep(const std::string& pattern, const std::string& path, int context) {
  std::string cmd = "grep -n";
  if (std::filesystem::is_directory(path)) {
    cmd += "r";
  }
  if (context > 0) {
    cmd += " -C " + std::to_string(context);
  }
  cmd += " \"" + pattern + "\" " + path;

  auto res = RunCommand(cmd);
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

absl::StatusOr<std::string> ToolExecutor::SearchCode(const std::string& query) { return Grep(query, ".", 0); }

absl::StatusOr<std::string> ToolExecutor::GitGrep(const nlohmann::json& args) {
  // Check if git is available
  auto git_check = ExecuteBash("git --version");
  if (!git_check.ok() || git_check->find("git version") == std::string::npos) {
    return "Error: git is not available on this system. git_grep_tool is not supported.";
  }

  // Check if it is a git repository
  auto git_repo_check = ExecuteBash("git rev-parse --is-inside-work-tree");
  if (!git_repo_check.ok() || git_repo_check->find("true") == std::string::npos) {
    return "Error: not a git repository. git_grep_tool is not supported.";
  }

  std::string cmd = "git grep";

  if (args.value("line_number", true)) cmd += " -n";
  if (args.value("case_insensitive", false)) cmd += " -i";
  if (args.value("count", false)) cmd += " -c";
  if (args.value("show_function", false)) cmd += " -p";
  if (args.value("function_context", false)) cmd += " -W";
  if (args.value("files_with_matches", false)) cmd += " -l";
  if (args.value("word_regexp", false)) cmd += " -w";
  if (args.value("pcre", false)) cmd += " -P";
  if (args.value("cached", false)) cmd += " --cached";
  if (args.value("all_match", false)) cmd += " --all-match";

  if (args.contains("context")) {
    cmd += " -C " + std::to_string(args["context"].get<int>());
  } else {
    if (args.contains("before")) cmd += " -B " + std::to_string(args["before"].get<int>());
    if (args.contains("after")) cmd += " -A " + std::to_string(args["after"].get<int>());
  }

  if (args.contains("branch")) {
    cmd += " " + args["branch"].get<std::string>();
  }

  cmd += " -e \"" + args["pattern"].get<std::string>() + "\"";

  if (args.contains("path")) {
    cmd += " -- \"" + args["path"].get<std::string>() + "\"";
  }

  auto res = RunCommand(cmd);
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

absl::StatusOr<std::string> ToolExecutor::SaveMemo(const std::string& content, const std::vector<std::string>& tags) {
  nlohmann::json tags_json = tags;
  auto status = db_->AddMemo(content, tags_json.dump());
  if (!status.ok()) return status;
  return "Memo saved successfully.";
}

absl::StatusOr<std::string> ToolExecutor::RetrieveMemos(const std::vector<std::string>& tags) {
  auto memos_or = db_->GetMemosByTags(tags);
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
  return result.dump(2);
}

absl::StatusOr<std::string> ToolExecutor::ListDirectory(const nlohmann::json& args) {
  std::string path = args.contains("path") ? args["path"].get<std::string>() : ".";
  int max_depth = args.contains("depth") ? args["depth"].get<int>() : 1;
  bool git_only = args.contains("git_only") ? args["git_only"].get<bool>() : true;

  auto git_repo_check = ExecuteBash("git rev-parse --is-inside-work-tree");
  if (git_only && git_repo_check.ok() && git_repo_check->find("true") != std::string::npos) {
    std::string cmd = "git ls-files --cached --others --exclude-standard";
    if (path != ".") {
      cmd += " " + path;
    }
    auto git_res = ExecuteBash(cmd);
    if (git_res.ok()) {
      return git_res;
    }
  }

  // Fallback to std::filesystem
  std::stringstream ss;
  if (!std::filesystem::exists(path)) return absl::NotFoundError("Directory not found: " + path);

  for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
    auto relative = std::filesystem::relative(entry.path(), path);
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

absl::StatusOr<std::string> ToolExecutor::ManageScratchpad(const nlohmann::json& args) {
  if (session_id_.empty()) return absl::FailedPreconditionError("No active session");
  std::string action = args.contains("action") ? args["action"].get<std::string>() : "read";

  if (action == "read") {
    auto res = db_->GetScratchpad(session_id_);
    if (!res.ok()) {
      if (absl::IsNotFound(res.status())) return "Scratchpad is empty.";
      return res.status();
    }
    if (res->empty()) return "Scratchpad is empty.";
    return *res;
  }
  if (action == "update") {
    if (!args.contains("content")) return absl::InvalidArgumentError("Missing 'content' for update");
    std::string content = args["content"].get<std::string>();
    auto status = db_->UpdateScratchpad(session_id_, content);
    if (!status.ok()) return status;
    return "Scratchpad updated.";
  }
  if (action == "append") {
    if (!args.contains("content")) return absl::InvalidArgumentError("Missing 'content' for append");
    std::string content = args["content"].get<std::string>();
    auto current = db_->GetScratchpad(session_id_);
    std::string new_content = (current.ok() ? *current : "") + content;
    auto status = db_->UpdateScratchpad(session_id_, new_content);
    if (!status.ok()) return status;
    return "Content appended to scratchpad.";
  }
  return absl::InvalidArgumentError("Unknown action: " + action);
}

absl::StatusOr<std::string> ToolExecutor::DescribeDb() {
  return db_->Query("SELECT name, sql FROM sqlite_master WHERE type='table'");
}

absl::StatusOr<std::string> ToolExecutor::UseSkill(const nlohmann::json& args) {
  if (!args.contains("name")) return absl::InvalidArgumentError("Missing 'name' argument");
  std::string name = args["name"].get<std::string>();
  std::string action = args.contains("action") ? args["action"].get<std::string>() : "activate";

  if (session_id_.empty()) return absl::FailedPreconditionError("No active session");

  auto active_skills_or = db_->GetActiveSkills(session_id_);
  if (!active_skills_or.ok()) return active_skills_or.status();
  std::vector<std::string> active_skills = *active_skills_or;

  if (action == "activate") {
    // Increment count
    auto status = db_->IncrementSkillActivationCount(name);
    if (!status.ok()) return status;

    // Add to active if not present
    if (std::find(active_skills.begin(), active_skills.end(), name) == active_skills.end()) {
      active_skills.push_back(name);
      status = db_->SetActiveSkills(session_id_, active_skills);
      if (!status.ok()) return status;
    }

    // Return patch
    auto skills_or = db_->GetSkills();
    if (!skills_or.ok()) return skills_or.status();
    for (const auto& s : *skills_or) {
      if (s.name == name) {
        return "Skill '" + name + "' activated.\n\n" + s.system_prompt_patch;
      }
    }
    return absl::NotFoundError("Skill not found: " + name);
  }

  if (action == "deactivate") {
    auto it = std::find(active_skills.begin(), active_skills.end(), name);
    if (it != active_skills.end()) {
      active_skills.erase(it);
      auto status = db_->SetActiveSkills(session_id_, active_skills);
      if (!status.ok()) return status;
      return "Skill '" + name + "' deactivated.";
    }
    return "Skill '" + name + "' was not active.";
  }

  return absl::InvalidArgumentError("Unknown action: " + action);
}

}  // namespace slop
