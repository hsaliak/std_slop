#ifndef SLOP_CORE_LLM_QUERY_OPTIONS_H_
#define SLOP_CORE_LLM_QUERY_OPTIONS_H_

#include <optional>
#include <string>

namespace slop {

struct LlmQueryOptions {
  enum class ExecutionScope {
    kRoot,
    kSubquery,
  };

  std::string session_id = "query";
  std::optional<std::string> skill;
  std::optional<int> context_window;
  ExecutionScope execution_scope = ExecutionScope::kRoot;
  int execution_depth = 0;
};

}  // namespace slop

#endif  // SLOP_CORE_LLM_QUERY_OPTIONS_H_
