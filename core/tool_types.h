#ifndef SLOP_SQL_TOOL_TYPES_H_
#define SLOP_SQL_TOOL_TYPES_H_

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "nlohmann/json.hpp"

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

struct ApplyPatchRequest {
  struct Patch {
    std::string find;
    std::string replace;
  };
  std::string path;
  std::vector<Patch> patches;
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
  std::string action = "read";  // "read", "update", "append"
  std::optional<std::string> content;
};

struct UseSkillRequest {
  std::string name;
  std::string action = "activate";  // "activate", "deactivate"
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

}  // namespace slop

namespace nlohmann {

template <typename T>
struct adl_serializer<std::optional<T>> {
  static void to_json(json& j, const std::optional<T>& opt) {
    if (opt.has_value()) {
      j = *opt;
    } else {
      j = nullptr;
    }
  }

  static void from_json(const json& j, std::optional<T>& opt) {
    if (j.is_null()) {
      opt = std::nullopt;
    } else {
      opt = j.get<T>();
    }
  }
};

// We need to define to_json/from_json for our structs here or in slop namespace if they are used with get<T>()
}  // namespace nlohmann

namespace slop {

// Define from_json for each request struct to avoid using the macro which might have issues with optional
inline void from_json(const nlohmann::json& j, ReadFileRequest& r) {
  r.path = j.at("path").get<std::string>();
  if (j.contains("start_line")) r.start_line = j.at("start_line").get<std::optional<int>>();
  if (j.contains("end_line")) r.end_line = j.at("end_line").get<std::optional<int>>();
  r.add_line_numbers = j.value("add_line_numbers", true);
}

inline void from_json(const nlohmann::json& j, WriteFileRequest& r) {
  r.path = j.at("path").get<std::string>();
  r.content = j.at("content").get<std::string>();
}

inline void from_json(const nlohmann::json& j, ApplyPatchRequest::Patch& p) {
  p.find = j.at("find").get<std::string>();
  p.replace = j.at("replace").get<std::string>();
}

inline void from_json(const nlohmann::json& j, ApplyPatchRequest& r) {
  r.path = j.at("path").get<std::string>();
  r.patches = j.at("patches").get<std::vector<ApplyPatchRequest::Patch>>();
}

inline void from_json(const nlohmann::json& j, GrepRequest& r) {
  r.pattern = j.at("pattern").get<std::string>();
  r.path = j.value("path", ".");
  r.context = j.value("context", 0);
}

inline void from_json(const nlohmann::json& j, GitGrepRequest& r) {
  if (j.contains("pattern")) r.pattern = j.at("pattern").get<std::optional<std::string>>();
  if (j.contains("patterns")) r.patterns = j.at("patterns").get<std::vector<std::string>>();
  if (j.contains("path")) {
    if (j.at("path").is_array()) {
      r.path = j.at("path").get<std::vector<std::string>>();
    } else {
      r.path = {j.at("path").get<std::string>()};
    }
  } else {
    r.path = {"."};
  }
  if (j.contains("branch")) r.branch = j.at("branch").get<std::optional<std::string>>();
  r.case_insensitive = j.value("case_insensitive", false);
  r.word_regexp = j.value("word_regexp", false);
  r.line_number = j.value("line_number", true);
  r.files_with_matches = j.value("files_with_matches", false);
  r.count = j.value("count", false);
  r.show_function = j.value("show_function", false);
  r.cached = j.value("cached", false);
  r.all_match = j.value("all_match", false);
  r.pcre = j.value("pcre", false);
  r.function_context = j.value("function_context", false);
  r.untracked = j.value("untracked", false);
  r.no_index = j.value("no_index", false);
  r.exclude_standard = j.value("exclude_standard", true);
  r.fixed_strings = j.value("fixed_strings", false);
  if (j.contains("max_depth")) r.max_depth = j.at("max_depth").get<std::optional<int>>();
  if (j.contains("context")) r.context = j.at("context").get<std::optional<int>>();
  if (j.contains("before")) r.before = j.at("before").get<std::optional<int>>();
  if (j.contains("after")) r.after = j.at("after").get<std::optional<int>>();
}

inline void from_json(const nlohmann::json& j, ExecuteBashRequest& r) {
  r.command = j.at("command").get<std::string>();
}

inline void from_json(const nlohmann::json& j, QueryDbRequest& r) { r.sql = j.at("sql").get<std::string>(); }

inline void from_json(const nlohmann::json& j, SaveMemoRequest& r) {
  r.content = j.at("content").get<std::string>();
  r.tags = j.at("tags").get<std::vector<std::string>>();
}

inline void from_json(const nlohmann::json& j, RetrieveMemosRequest& r) {
  r.tags = j.at("tags").get<std::vector<std::string>>();
}

inline void from_json(const nlohmann::json& j, ListDirectoryRequest& r) {
  r.path = j.value("path", ".");
  if (j.contains("depth")) r.depth = j.at("depth").get<std::optional<int>>();
  r.git_only = j.value("git_only", false);
}

inline void from_json(const nlohmann::json& j, ManageScratchpadRequest& r) {
  r.action = j.value("action", "read");
  if (j.contains("content")) r.content = j.at("content").get<std::optional<std::string>>();
}

inline void from_json(const nlohmann::json& j, UseSkillRequest& r) {
  r.name = j.at("name").get<std::string>();
  r.action = j.value("action", "activate");
}

inline void from_json(const nlohmann::json& j, SearchCodeRequest& r) { r.query = j.at("query").get<std::string>(); }

inline void from_json(const nlohmann::json& j, GitBranchStagingRequest& r) {
  r.name = j.at("name").get<std::string>();
  r.base_branch = j.value("base_branch", "");
}

inline void from_json(const nlohmann::json& j, GitCommitPatchRequest& r) {
  r.summary = j.at("summary").get<std::string>();
  r.rationale = j.at("rationale").get<std::string>();
}

inline void from_json(const nlohmann::json& j, GitFormatPatchSeriesRequest& r) {
  r.base_branch = j.value("base_branch", "");
}

inline void from_json(const nlohmann::json& j, GitFinalizeSeriesRequest& r) {
  r.target_branch = j.value("target_branch", "");
}

inline void from_json(const nlohmann::json& j, GitVerifySeriesRequest& r) {
  r.command = j.at("command").get<std::string>();
  r.base_branch = j.value("base_branch", "");
}

inline void from_json(const nlohmann::json& j, GitRerollPatchRequest& r) {
  r.index = j.at("index").get<int>();
  r.base_branch = j.value("base_branch", "");
}

}  // namespace slop

#endif  // SLOP_SQL_TOOL_TYPES_H_
