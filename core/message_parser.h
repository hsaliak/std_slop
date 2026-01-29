#ifndef SLOP_SQL_CORE_MESSAGE_PARSER_H_
#define SLOP_SQL_CORE_MESSAGE_PARSER_H_

#include <string>
#include <vector>

#include "absl/status/statusor.h"

#include "core/database.h"
#include "core/orchestrator_strategy.h"

namespace slop {

// Shared utility for extracting tool calls from Database::Message objects
// regardless of the underlying JSON format (OpenAI vs Gemini).
class MessageParser {
 public:
  // Extracts ToolCall objects from a message based on its parsing_strategy.
  static absl::StatusOr<std::vector<ToolCall>> ExtractToolCalls(const Database::Message& msg);

  // Extracts any assistant text content from a JSON-encoded tool_call message.
  static std::string ExtractAssistantText(const Database::Message& msg);
};

}  // namespace slop

#endif  // SLOP_SQL_CORE_MESSAGE_PARSER_H_
