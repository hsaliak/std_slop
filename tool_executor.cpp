#include "tool_executor.h"
#include <fstream>
#include <sstream>
#include <cstdio>
#include <memory>
#include <array>
#include <iostream>
#include "absl/status/status.h"

#include <filesystem>

namespace slop {

absl::StatusOr<std::string> ToolExecutor::Execute(const std::string& name, const nlohmann::json& args) {
  if (name == "read_file") {
    if (!args.contains("path")) return absl::InvalidArgumentError("Missing 'path' argument");
    return ReadFile(args["path"], true);
  } else if (name == "write_file") {
    if (!args.contains("path")) return absl::InvalidArgumentError("Missing 'path' argument");
    if (!args.contains("content")) return absl::InvalidArgumentError("Missing 'content' argument");
    return WriteFile(args["path"], args["content"]);
  } else if (name == "grep_tool") {
    if (!args.contains("pattern")) return absl::InvalidArgumentError("Missing 'pattern' argument");
    
    std::string path = args.contains("path") ? args["path"].get<std::string>() : ".";
    int context = args.contains("context") ? args["context"].get<int>() : 0;

    // Delegate to GitGrep if in a git repo
    auto git_repo_check = ExecuteBash("git rev-parse --is-inside-work-tree");
    if (git_repo_check.ok() && git_repo_check->find("true") != std::string::npos) {
        auto git_res = GitGrep(args);
        // If git grep succeeded and found something, return it.
        // Otherwise fall back to standard grep (might be an untracked file).
        if (git_res.ok() && !git_res->empty() && git_res->find("Error:") == std::string::npos) {
            return git_res;
        }
    }

    return Grep(args["pattern"], path, context);
  } else if (name == "git_grep_tool") {
    return GitGrep(args);
  } else if (name == "execute_bash") {
    if (!args.contains("command")) return absl::InvalidArgumentError("Missing 'command' argument");
    return ExecuteBash(args["command"]);
  } else if (name == "search_code") {
    if (!args.contains("query")) return absl::InvalidArgumentError("Missing 'query' argument");
    nlohmann::json grep_args = args;
    grep_args["pattern"] = args["query"];
    return Execute("grep_tool", grep_args);
  } else if (name == "query_db") {
    if (!args.contains("sql")) return absl::InvalidArgumentError("Missing 'sql' argument");
    return db_->Query(args["sql"]);
  }
  return absl::NotFoundError("Tool not found: " + name);
}

absl::StatusOr<std::string> ToolExecutor::ReadFile(const std::string& path, bool add_line_numbers) {
  std::ifstream file(path);
  if (!file.is_open()) return absl::NotFoundError("Could not open file: " + path);
  
  std::stringstream ss;
  std::string line;
  int line_num = 1;
  while (std::getline(file, line)) {
    if (add_line_numbers) {
      ss << line_num++ << ": " << line << "\n";
    } else {
      ss << line << "\n";
    }
  }
  return ss.str();
}

absl::StatusOr<std::string> ToolExecutor::WriteFile(const std::string& path, const std::string& content) {
  std::ofstream file(path);
  if (!file.is_open()) return absl::InternalError("Could not open file for writing: " + path);
  file << content;
  return "File written successfully: " + path;
}

absl::StatusOr<std::string> ToolExecutor::ExecuteBash(const std::string& command) {
  std::array<char, 128> buffer;
  std::string result;
  std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
  if (!pipe) return absl::InternalError("popen() failed!");
  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    result += buffer.data();
  }
  return result;
}

absl::StatusOr<std::string> ToolExecutor::Grep(const std::string& pattern, const std::string& path, int context) {
  std::string cmd = "grep -rn";
  if (context > 0) cmd += " -C " + std::to_string(context);
  cmd += " \"" + pattern + "\" " + path;
  return ExecuteBash(cmd);
}

absl::StatusOr<std::string> ToolExecutor::SearchCode(const std::string& query) {
  nlohmann::json args;
  args["pattern"] = query;
  return Execute("grep_tool", args);
}

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

  return ExecuteBash(cmd);
}

} // namespace slop
