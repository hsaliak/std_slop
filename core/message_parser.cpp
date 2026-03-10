#include "core/message_parser.h"

#include "absl/log/log.h"
#include "absl/status/status.h"

#include "json_utils.h"

#include <nlohmann/json.hpp>

namespace slop {

MessageContext::MessageContext(const Database::Message& msg) : msg_(msg) {}

void MessageContext::EnsureParsed() const {
  if (parsed_) return;
  auto j_opt = json_parse(msg_.content);
  if (j_opt) {
    json_ = *j_opt;
    valid_ = true;
  } else {
    valid_ = false;
  }
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
    if (auto tool_calls = json_get<nlohmann::json::array_t>(j, "tool_calls")) {
      for (const auto& call : *tool_calls) {
        ToolCall tc;
        tc.id = json_get_or(call, "id", std::string{});
        if (const auto* fn = json_at(call, "function")) {
          tc.name = json_get_or(*fn, "name", std::string("unknown"));
          if (const auto* arguments = json_at(*fn, "arguments")) {
            if (arguments->is_string()) {
              std::string args_str = json_getter<std::string>::get(*arguments).value_or("{}");
              tc.args = json_parse(args_str).value_or(nlohmann::json::object());
            } else if (arguments->is_object() || arguments->is_array()) {
              tc.args = *arguments;
            } else {
              tc.args = nlohmann::json::object();
            }
          } else {
            tc.args = nlohmann::json::object();
          }
        }
        calls.push_back(tc);
      }
    }
  } else if (msg.parsing_strategy == "gemini" || msg.parsing_strategy == "gemini_gca") {
    ToolCall tc;
    tc.id = msg.tool_call_id;
    tc.name = msg.tool_call_id;  // Default to ID if name not in JSON

    if (const auto* fc = json_at(j, "functionCall")) {
      tc.name = json_get_or(*fc, "name", tc.name);
      if (const auto* args = json_at(*fc, "args")) {
        tc.args = *args;
      }
    } else if (const auto* args = json_at(j, "args")) {
      tc.args = *args;
    }
    calls.push_back(tc);
  } else {
    // Default fallback for unidentified strategies
    if (auto f_calls = json_get<nlohmann::json::array_t>(j, "functionCalls")) {
      for (const auto& call : *f_calls) {
        ToolCall tc;
        tc.name = json_get_or(call, "name", std::string("unknown"));
        if (const auto* args = json_at(call, "args")) {
          tc.args = *args;
        } else {
          tc.args = nlohmann::json::object();
        }
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
  return json_get_or(j, "content", std::string{});
}

}  // namespace slop
