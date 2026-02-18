#ifndef SLOP_SQL_TOOL_TYPES_H_
#define SLOP_SQL_TOOL_TYPES_H_

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "nlohmann/json.hpp"
#include "json_utils.h"

namespace slop {

struct ReadFileRequest {
  std::string path;
  std::optional<int> start_line;
  std::optional<int> end_line;
  bool add_line_numbers = true;
};

struct WriteFileRequest {
  std::string path;
  std::string content;
};

struct GrepRequest {
  std::string pattern;
  std::string path = ".";
  int context = 0;
};

struct GitGrepRequest {
  std::optional<std::string> pattern;
  std::vector<std::string> patterns;
  std::vector<std::string> path;
  std::optional<std::string> branch;
  bool case_insensitive = false;
  bool word_regexp = false;
  bool line_number = true;
  bool files_with_matches = false;
  bool count = false;
  bool show_function = false;
  bool cached = false;
  bool all_match = false;
  bool pcre = false;
  bool function_context = false;
  bool untracked = false;
  bool no_index = false;
  bool exclude_standard = true;
  bool fixed_strings = false;
  std::optional<int> max_depth;
  std::optional<int> context;
  std::optional<int> before;
  std::optional<int> after;
};

struct ExecuteBashRequest {
  std::string command;
  std::string input;
};

struct QueryDbRequest {
  std::string sql;
};

struct SaveMemoRequest {
  std::string content;
  std::vector<std::string> tags;
};

struct RetrieveMemosRequest {
  std::vector<std::string> tags;
};

struct ListDirectoryRequest {
  std::string path = ".";
  std::optional<int> depth;
  bool git_only = false;
};

struct ManageScratchpadRequest {
  std::string action;
  std::string key;
  std::optional<std::string> content;
};

struct UseSkillRequest {
  std::string name;
  std::string action;
};

struct SearchCodeRequest {
  std::string query;
};

struct GitBranchStagingRequest {
  std::string name;
  std::string base_branch;
};

struct GitCommitPatchRequest {
  std::string summary;
  std::string rationale;
};

struct GitFormatPatchSeriesRequest {
  std::string base_branch;
};

struct GitFinalizeSeriesRequest {
  std::string target_branch;
};

struct GitVerifySeriesRequest {
  std::string command;
  std::string base_branch;
};

struct GitRerollPatchRequest {
  int index;
  std::string base_branch;
};

struct RunLuaRequest {
  std::string script;
  nlohmann::json args;
};

inline void from_json(const nlohmann::json& j, ReadFileRequest& r) {
  r.path = json_get_or(j, "path", std::string{});
  r.start_line = json_get_or(j, "start_line", std::optional<int>{});
  r.end_line = json_get_or(j, "end_line", std::optional<int>{});
  r.add_line_numbers = json_get_or(j, "add_line_numbers", true);
}

inline void from_json(const nlohmann::json& j, WriteFileRequest& r) {
  r.path = json_get_or(j, "path", std::string{});
  r.content = json_get_or(j, "content", std::string{});
}

inline void from_json(const nlohmann::json& j, GrepRequest& r) {
  r.pattern = json_get_or(j, "pattern", std::string{});
  r.path = json_get_or(j, "path", std::string{"."});
  r.context = json_get_or(j, "context", 0);
}

inline void from_json(const nlohmann::json& j, GitGrepRequest& r) {
  r.pattern = json_get_or(j, "pattern", std::optional<std::string>{});
  r.patterns = json_get_or(j, "patterns", std::vector<std::string>{});
  if (j.contains("path")) {
    auto path_opt = json_get<std::vector<std::string>>(j, "path");
    if (path_opt) {
      r.path = *path_opt;
    } else {
      auto path_single = json_get<std::string>(j, "path");
      if (path_single) {
        r.path = {*path_single};
      } else {
        r.path = {"."};
      }
    }
  } else {
    r.path = {"."};
  }
  r.branch = json_get_or(j, "branch", std::optional<std::string>{});
  r.case_insensitive = json_get_or(j, "case_insensitive", false);
  r.word_regexp = json_get_or(j, "word_regexp", false);
  r.line_number = json_get_or(j, "line_number", true);
  r.files_with_matches = json_get_or(j, "files_with_matches", false);
  r.count = json_get_or(j, "count", false);
  r.show_function = json_get_or(j, "show_function", false);
  r.cached = json_get_or(j, "cached", false);
  r.all_match = json_get_or(j, "all_match", false);
  r.pcre = json_get_or(j, "pcre", false);
  r.function_context = json_get_or(j, "function_context", false);
  r.untracked = json_get_or(j, "untracked", false);
  r.no_index = json_get_or(j, "no_index", false);
  r.exclude_standard = json_get_or(j, "exclude_standard", true);
  r.fixed_strings = json_get_or(j, "fixed_strings", false);
  r.max_depth = json_get_or(j, "max_depth", std::optional<int>{});
  r.context = json_get_or(j, "context", std::optional<int>{});
  r.before = json_get_or(j, "before", std::optional<int>{});
  r.after = json_get_or(j, "after", std::optional<int>{});
}

inline void from_json(const nlohmann::json& j, ExecuteBashRequest& r) {
  r.command = json_get_or(j, "command", std::string{});
  r.input = json_get_or(j, "input", std::string{});
}

inline void from_json(const nlohmann::json& j, QueryDbRequest& r) {
  r.sql = json_get_or(j, "sql", std::string{});
}

inline void from_json(const nlohmann::json& j, SaveMemoRequest& r) {
  r.content = json_get_or(j, "content", std::string{});
  r.tags = json_get_or(j, "tags", std::vector<std::string>{});
}

inline void from_json(const nlohmann::json& j, RetrieveMemosRequest& r) {
  r.tags = json_get_or(j, "tags", std::vector<std::string>{});
}

inline void from_json(const nlohmann::json& j, ListDirectoryRequest& r) {
  r.path = json_get_or(j, "path", std::string{"."});
  r.depth = json_get_or(j, "depth", std::optional<int>{});
  r.git_only = json_get_or(j, "git_only", false);
}

inline void from_json(const nlohmann::json& j, ManageScratchpadRequest& r) {
  r.action = json_get_or(j, "action", std::string{"read"});
  r.key = json_get_or(j, "key", std::string{});
  r.content = json_get_or(j, "content", std::optional<std::string>{});
}

inline void from_json(const nlohmann::json& j, UseSkillRequest& r) {
  r.name = json_get_or(j, "name", std::string{});
  r.action = json_get_or(j, "action", std::string{"activate"});
}

inline void from_json(const nlohmann::json& j, SearchCodeRequest& r) {
  r.query = json_get_or(j, "query", std::string{});
}

inline void from_json(const nlohmann::json& j, GitBranchStagingRequest& r) {
  r.name = json_get_or(j, "name", std::string{});
  r.base_branch = json_get_or(j, "base_branch", std::string{});
}

inline void from_json(const nlohmann::json& j, GitCommitPatchRequest& r) {
  r.summary = json_get_or(j, "summary", std::string{});
  r.rationale = json_get_or(j, "rationale", std::string{});
}

inline void from_json(const nlohmann::json& j, GitFormatPatchSeriesRequest& r) {
  r.base_branch = json_get_or(j, "base_branch", std::string{});
}

inline void from_json(const nlohmann::json& j, GitFinalizeSeriesRequest& r) {
  r.target_branch = json_get_or(j, "target_branch", std::string{});
}

inline void from_json(const nlohmann::json& j, GitVerifySeriesRequest& r) {
  r.command = json_get_or(j, "command", std::string{});
  r.base_branch = json_get_or(j, "base_branch", std::string{});
}

inline void from_json(const nlohmann::json& j, GitRerollPatchRequest& r) {
  r.index = json_get_or(j, "index", 0);
  r.base_branch = json_get_or(j, "base_branch", std::string{});
}

inline void from_json(const nlohmann::json& j, RunLuaRequest& r) {
  r.script = json_get_or(j, "script", std::string{});
  r.args = json_get_or(j, "args", nlohmann::json{});
}

}  // namespace slop

#endif  // SLOP_SQL_TOOL_TYPES_H_
