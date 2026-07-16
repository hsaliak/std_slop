#ifndef SLOP_CORE_RESPONSES_TYPES_H_
#define SLOP_CORE_RESPONSES_TYPES_H_

#include <optional>
#include <string>

#include "nlohmann/json.hpp"

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

// Preserves a completed Responses output item for the active turn, including
// provider fields not yet modeled by the message-centric persistence layer.
struct ResponsesOutputItem {
  std::string id;
  std::string type;
  std::string status;
  nlohmann::json raw;
};

struct ResponseUsage {
  int input_tokens = 0;
  int output_tokens = 0;
  std::optional<int> cached_input_tokens;
};


}  // namespace slop

#endif  // SLOP_CORE_RESPONSES_TYPES_H_
