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

struct ResponseUsage {
  int input_tokens = 0;
  int output_tokens = 0;
  std::optional<int> cached_input_tokens;
};

}  // namespace slop

#endif  // SLOP_CORE_RESPONSES_TYPES_H_
