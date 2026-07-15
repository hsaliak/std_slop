#ifndef SLOP_SQL_ORCHESTRATOR_STRATEGY_H_
#define SLOP_SQL_ORCHESTRATOR_STRATEGY_H_

#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"

#include "core/database.h"

#include <nlohmann/json.hpp>

namespace slop {

class Orchestrator;  // Forward declaration

struct ToolCall {
  std::string id;
  std::string name;
  nlohmann::json args;
};

struct ModelInfo {
  std::string id;
  std::string name;
};

struct ResponseUsage {
  int input_tokens = 0;
  int output_tokens = 0;
  std::optional<int> cached_input_tokens;
};


}  // namespace slop

#endif  // SLOP_SQL_ORCHESTRATOR_STRATEGY_H_
