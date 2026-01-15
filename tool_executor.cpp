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
    return ReadFile(args["path"]);
  } else if (name == "write_file") {
    if (!args.contains("path") || !args.contains("content")) {
      return absl::InvalidArgumentError("Missing 'path' or 'content' argument");
    }
    return WriteFile(args["path"], args["content"]);
  } else if (name == "execute_bash") {
    if (!args.contains("command")) return absl::InvalidArgumentError("Missing 'command' argument");
    return ExecuteBash(args["command"]);
  } else if (name == "search_code") {
    if (!args.contains("query")) return absl::InvalidArgumentError("Missing 'query' argument");
    return SearchCode(args["query"]);
  } else if (name == "index_directory") {
    if (!args.contains("path")) return absl::InvalidArgumentError("Missing 'path' argument");
    return IndexDirectory(args["path"]);
  } else if (name == "query_db") {
    if (!args.contains("sql")) return absl::InvalidArgumentError("Missing 'sql' argument");
    return QueryDb(args["sql"]);
  }
  return absl::NotFoundError("Tool not found: " + name);
}

absl::StatusOr<std::string> ToolExecutor::ReadFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return absl::NotFoundError("Could not open file: " + path);
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

absl::StatusOr<std::string> ToolExecutor::WriteFile(const std::string& path, const std::string& content) {
  std::ofstream file(path);
  if (!file.is_open()) {
    return absl::InternalError("Could not open file for writing: " + path);
  }
  file << content;
  return "File written successfully: " + path;
}

absl::StatusOr<std::string> ToolExecutor::ExecuteBash(const std::string& command) {
  std::array<char, 128> buffer;
  std::string result;
  std::string full_command = command + " 2>&1";
  std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(full_command.c_str(), "r"), pclose);
  if (!pipe) {
    return absl::InternalError("popen() failed!");
  }
  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    result += buffer.data();
  }
  return result;
}

absl::StatusOr<std::string> ToolExecutor::SearchCode(const std::string& query) {
  auto results_or = db_->SearchCode(query);
  if (!results_or.ok()) return results_or.status();

  nlohmann::json j_results = nlohmann::json::array();
  for (const auto& res : *results_or) {
    j_results.push_back({{"path", res.first}, {"snippet", res.second.substr(0, 200) + "..."}});
  }
  return j_results.dump(2);
}

absl::StatusOr<std::string> ToolExecutor::IndexDirectory(const std::string& path) {
    int count = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(path, ec)) {
        if (ec) break;
        if (entry.is_regular_file()) {
            auto content_or = ReadFile(entry.path().string());
            if (content_or.ok()) {
                auto status = db_->IndexFile(entry.path().string(), *content_or);
                if (status.ok()) count++;
            }
        }
    }
    if (ec) {
        return absl::InternalError("Indexing failed: " + ec.message());
    }
    return "Indexed " + std::to_string(count) + " files.";
}

absl::StatusOr<std::string> ToolExecutor::QueryDb(const std::string& sql) {
    return db_->Query(sql);
}

}  // namespace slop
