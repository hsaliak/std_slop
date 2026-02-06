#ifndef SLOP_SQL_ORCHESTRATOR_TYPES_H_
#define SLOP_SQL_ORCHESTRATOR_TYPES_H_

#include <string>
#include <vector>

#include "core/database.h"

#include <nlohmann/json.hpp>

namespace slop {

struct ToolCall {
  std::string id;
  std::string name;
  nlohmann::json args;
};

struct ModelInfo {
  std::string id;
  std::string name;
};

struct Usage {
  int prompt_tokens = 0;
  int completion_tokens = 0;
  int total_tokens = 0;
};

struct OrchestratorResponse {
  std::vector<Database::Message> new_messages;
  Usage usage;
};

struct QuotaInfo {
  nlohmann::json raw_data;
};

}  // namespace slop

#endif  // SLOP_SQL_ORCHESTRATOR_TYPES_H_
