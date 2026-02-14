#include "core/message_parser.h"

#include "absl/status/status.h"

#include <nlohmann/json.hpp>

namespace slop {

MessageContext::MessageContext(const Database::Message& msg) : msg_(msg) {}

void MessageContext::EnsureParsed() const {
  if (parsed_) return;
  json_ = nlohmann::json::parse(msg_.content, nullptr, false);
  valid_ = !json_.is_discarded();
  parsed_ = true;
}

const nlohmann::json& MessageContext::json() const {
  EnsureParsed();
  return json_;
}

bool MessageContext::is_valid() const {
  EnsureParsed();
  return valid_;
}

absl::StatusOr<std::vector<ToolCall>> MessageParser::ExtractToolCalls(const MessageContext& ctx) {
  const auto& msg = ctx.message();
  if (msg.status != "tool_call") return std::vector<ToolCall>();

  if (!ctx.is_valid()) {
    return absl::InternalError("Failed to parse message content as JSON");
  }

  const auto& j = ctx.json();
  std::vector<ToolCall> calls;

  if (msg.parsing_strategy == "openai") {
    if (j.contains("tool_calls") && j["tool_calls"].is_array()) {
      for (const auto& call : j["tool_calls"]) {
        ToolCall tc;
        tc.id = call.value("id", "");
        if (call.contains("function")) {
          tc.name = call["function"].value("name", "unknown");
          std::string args_str = call["function"].value("arguments", "{}");
          tc.args = nlohmann::json::parse(args_str, nullptr, false);
        }
        calls.push_back(tc);
      }
    }
  } else if (msg.parsing_strategy == "gemini" || msg.parsing_strategy == "gemini_gca") {
    ToolCall tc;
    tc.id = msg.tool_call_id;
    tc.name = msg.tool_call_id;  // Default to ID if name not in JSON

    if (j.contains("functionCall")) {
      tc.name = j["functionCall"].value("name", tc.name);
      if (j["functionCall"].contains("args")) {
        tc.args = j["functionCall"]["args"];
      }
    } else if (j.contains("args")) {
      tc.args = j["args"];
    }
    calls.push_back(tc);
  } else {
    // Default fallback for unidentified strategies
    if (j.contains("functionCalls") && j["functionCalls"].is_array()) {
      for (const auto& call : j["functionCalls"]) {
        ToolCall tc;
        tc.name = call.value("name", "unknown");
        tc.args = call.contains("args") ? call["args"] : nlohmann::json::object();
        calls.push_back(tc);
      }
    }
  }

  return calls;
}

std::string MessageParser::ExtractAssistantText(const MessageContext& ctx) {
  const auto& msg = ctx.message();
  if (msg.status != "tool_call") return msg.content;

  if (!ctx.is_valid()) return "";

  const auto& j = ctx.json();
  if (j.contains("content") && j["content"].is_string()) {
    return j["content"];
  }

  return "";
}

}  // namespace slop
