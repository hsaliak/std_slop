#ifndef SLOP_SQL_CORE_MESSAGE_PARSER_H_
#define SLOP_SQL_CORE_MESSAGE_PARSER_H_

#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

#include "core/database.h"
#include "core/orchestrator_strategy.h"

namespace slop {

// MessageContext wraps a Database::Message and lazily parses its content into
// JSON, caching the result to avoid redundant parsing in MessageParser methods.
class MessageContext {
 public:
  explicit MessageContext(const Database::Message& msg);

  // Non-copyable to ensure the cache is managed efficiently.
  MessageContext(const MessageContext&) = delete;
  MessageContext& operator=(const MessageContext&) = delete;

  const nlohmann::json& json() const;
  bool is_valid() const;
  const Database::Message& message() const { return msg_; }

 private:
  const Database::Message& msg_;
  mutable nlohmann::json json_;
  mutable bool parsed_ = false;
  mutable bool valid_ = false;

  void EnsureParsed() const;
};

// Shared utility for extracting tool calls from Responses-format Database::Message objects.
class MessageParser {
 public:
  // Extracts ToolCall objects from a Responses-format message.
  static absl::StatusOr<std::vector<ToolCall>> ExtractToolCalls(const MessageContext& ctx);

  // Extracts any assistant text content from a JSON-encoded tool_call message.
  static std::string ExtractAssistantText(const MessageContext& ctx);
};

}  // namespace slop

#endif  // SLOP_SQL_CORE_MESSAGE_PARSER_H_
