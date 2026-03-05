#include "core/orchestrator_openai_responses.h"

#include <cstdlib>
#include <optional>
#include <unordered_map>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_split.h"

#include "core/message_parser.h"
#include "core/openai_utils.h"
#include "core/orchestrator.h"
#include "json_utils.h"

namespace slop {
namespace {

struct ModelSelection {
  std::string base_model;
  std::string reasoning_effort;
};

absl::StatusOr<ModelSelection> ParseResponsesModelSelection(const std::string& selector) {
  static constexpr const char* kLow = "low";
  static constexpr const char* kMedium = "medium";
  static constexpr const char* kHigh = "high";

  const size_t split = selector.rfind(':');
  if (split == std::string::npos) {
    return ModelSelection{selector, kMedium};
  }

  const std::string base_model = selector.substr(0, split);
  const std::string suffix = selector.substr(split + 1);
  if (suffix == kLow || suffix == kMedium || suffix == kHigh) {
    if (base_model.empty()) {
      return absl::InvalidArgumentError("Model selector base cannot be empty before reasoning suffix");
    }
    return ModelSelection{base_model, suffix};
  }

  return absl::InvalidArgumentError("Invalid reasoning level in model selector. Use low, medium, or high.");
}

bool IsDebugToolsEnabled() { return std::getenv("SLOP_DEBUG_TOOLS") != nullptr; }

std::optional<nlohmann::json> TryNormalizeSseResponsesPayload(const std::string& payload) {
  if (!absl::StrContains(payload, "event:") || !absl::StrContains(payload, "data:")) {
    return std::nullopt;
  }

  nlohmann::json output = nlohmann::json::array();
  std::unordered_map<std::string, size_t> output_index_by_key;
  nlohmann::json usage;
  bool saw_stream_event = false;
  std::string output_text_delta;

  const auto upsert_output_item = [&](const nlohmann::json& item) {
    const std::string type = json_get_or(item, "type", std::string{});
    if (type != "function_call") {
      output.push_back(item);
      return;
    }
    std::string call_id = json_get_or(item, "call_id", std::string{});
    if (call_id.empty()) {
      call_id = json_get_or(item, "id", std::string{});
    }
    if (call_id.empty()) {
      output.push_back(item);
      return;
    }

    const std::string key = "function_call:" + call_id;
    const auto it = output_index_by_key.find(key);
    if (it == output_index_by_key.end()) {
      output_index_by_key.emplace(key, output.size());
      output.push_back(item);
      return;
    }

    const auto* incoming_args = json_at(item, "arguments");
    const auto* existing_args = json_at(output[it->second], "arguments");
    bool prefer_incoming = false;
    if (existing_args == nullptr && incoming_args != nullptr) {
      prefer_incoming = true;
    } else if (existing_args != nullptr && incoming_args != nullptr) {
      if (existing_args->is_string() && incoming_args->is_string()) {
        const std::string existing_str = json_getter<std::string>::get(*existing_args).value_or(std::string{});
        const std::string incoming_str = json_getter<std::string>::get(*incoming_args).value_or(std::string{});
        prefer_incoming = incoming_str.size() > existing_str.size();
      } else if ((existing_args->is_object() || existing_args->is_array()) && incoming_args->is_string()) {
        prefer_incoming = false;
      } else if (existing_args->is_string() && (incoming_args->is_object() || incoming_args->is_array())) {
        prefer_incoming = true;
      }
    }
    if (prefer_incoming) {
      output[it->second] = item;
    }
  };

  std::string event_data;
  const auto flush_event = [&]() {
    if (event_data.empty()) {
      return;
    }
    auto evt_opt = json_parse(event_data);
    event_data.clear();
    if (!evt_opt) {
      return;
    }
    saw_stream_event = true;
    const auto& evt = *evt_opt;
    const std::string type = json_get_or(evt, "type", std::string{});
    if (type == "response.output_item.done" || type == "response.output_item.added") {
      const auto* item = json_at(evt, "item");
      if (item != nullptr) {
        upsert_output_item(*item);
      }
      return;
    }
    if (type == "response.output_text.delta") {
      const std::string delta = json_get_or(evt, "delta", std::string{});
      if (!delta.empty()) {
        output_text_delta.append(delta);
      }
      return;
    }
    if (type == "response.completed" || type == "response.done") {
      const auto* response = json_at(evt, "response");
      if (response != nullptr) {
        const auto* usage_obj = json_at(*response, "usage");
        if (usage_obj != nullptr) {
          usage = *usage_obj;
        }
      }
    }
  };

  for (const auto& raw_line : absl::StrSplit(payload, '\n')) {
    std::string line(raw_line);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      flush_event();
      continue;
    }
    if (absl::StartsWith(line, "event:")) {
      continue;
    }
    if (absl::StartsWith(line, "data:")) {
      std::string data_piece = std::string(absl::StripLeadingAsciiWhitespace(line.substr(5)));
      if (data_piece == "[DONE]") {
        flush_event();
        continue;
      }
      if (!event_data.empty()) {
        event_data.push_back('\n');
      }
      event_data.append(data_piece);
    }
  }
  flush_event();

  if (!saw_stream_event) {
    return std::nullopt;
  }
  if (!output_text_delta.empty()) {
    output.push_back({{"type", "message"},
                      {"role", "assistant"},
                      {"content", nlohmann::json::array({{{"type", "output_text"}, {"text", output_text_delta}}})}});
  }

  nlohmann::json normalized = {{"output", output}};
  if (!usage.is_null() && !usage.empty()) {
    normalized["usage"] = usage;
  }
  return normalized;
}

}  // namespace

OpenAiResponsesOrchestrator::OpenAiResponsesOrchestrator(Database* db, HttpClient* http_client,
                                                         const std::string& model, const std::string& base_url)
    : db_(db), http_client_(http_client), model_(model), base_url_(base_url) {}

absl::StatusOr<nlohmann::json> OpenAiResponsesOrchestrator::AssemblePayload(
    const std::string& session_id, const std::string& system_instruction,
    const std::vector<Database::Message>& history) {
  (void)session_id;
  const auto model_selection_or = ParseResponsesModelSelection(model_);
  if (!model_selection_or.ok()) {
    return model_selection_or.status();
  }
  const ModelSelection& model_selection = *model_selection_or;

  nlohmann::json input = nlohmann::json::array();

  const auto enabled_tool_names = GetEnabledToolNames(db_);

  for (size_t i = 0; i < history.size(); ++i) {
    const auto& msg = history[i];
    if (msg.role == "system") {
      continue;
    }

    std::string display_content = msg.content;
    if (i == 0) {
      display_content = "## Begin Conversation History\n" + display_content;
    }
    if (i == history.size() - 1 && msg.role == "user" && i > 0) {
      display_content = "## End of History\n\n### CURRENT REQUEST\n" + display_content;
    }

    if (msg.status == "tool_call") {
      auto j_opt = json_parse(msg.content);
      if (!j_opt) {
        continue;
      }
      auto tool_calls = json_get<nlohmann::json::array_t>(*j_opt, "tool_calls");
      if (!tool_calls) {
        continue;
      }
      for (const auto& tc : *tool_calls) {
        const auto* fn = json_at(tc, "function");
        if (fn == nullptr) {
          continue;
        }
        const std::string name = json_get_or(*fn, "name", std::string{});
        if (!enabled_tool_names.contains(name)) {
          LOG(WARNING) << "Filtering invalid tool call from history: " << name;
          continue;
        }
        input.push_back({{"type", "function_call"},
                         {"call_id", json_get_or(tc, "id", std::string{})},
                         {"name", name},
                         {"arguments", json_get_or(*fn, "arguments", std::string("{}"))}});
      }
      continue;
    }

    if (msg.role == "tool") {
      const std::string call_id = msg.tool_call_id.substr(0, msg.tool_call_id.find('|'));
      const std::string tool_name =
          absl::StrContains(msg.tool_call_id, '|') ? msg.tool_call_id.substr(msg.tool_call_id.find('|') + 1) : "";
      if (!tool_name.empty() && !enabled_tool_names.contains(tool_name)) {
        LOG(WARNING) << "Filtering invalid tool response from history: " << tool_name;
        continue;
      }
      input.push_back({{"type", "function_call_output"}, {"call_id", call_id}, {"output", msg.content}});
      continue;
    }

    input.push_back({{"role", msg.role}, {"content", display_content}});
  }

  nlohmann::json payload = {{"model", model_selection.base_model}, {"input", input}, {"store", false}};
  if (!system_instruction.empty()) {
    payload["instructions"] = system_instruction;
  }
  if (absl::StrContains(base_url_, "/backend-api/codex")) {
    payload["reasoning"] = {{"effort", model_selection.reasoning_effort}, {"summary", "auto"}};
    payload["stream"] = true;
  }
  const nlohmann::json tools = BuildOpenAiResponsesTools(db_);
  if (!tools.empty()) {
    payload["tools"] = tools;
  }
  if (IsDebugToolsEnabled()) {
    const size_t input_count = input.is_array() ? input.size() : 0;
    const size_t tools_count = tools.is_array() ? tools.size() : 0;
    LOG(INFO) << "[tool_debug] Responses AssemblePayload session=" << session_id << " model=" << model_
              << " input_items=" << input_count << " tools=" << tools_count
              << " stream=" << json_get_or(payload, "stream", false);
  }
  return payload;
}

absl::StatusOr<int> OpenAiResponsesOrchestrator::ProcessResponse(const std::string& session_id,
                                                                 const std::string& response_json,
                                                                 const std::string& group_id) {
  auto j_opt = json_parse(response_json);
  if (!j_opt) {
    auto sse_normalized = TryNormalizeSseResponsesPayload(response_json);
    if (sse_normalized.has_value()) {
      j_opt = sse_normalized;
    }
  }
  if (!j_opt) {
    LOG(ERROR) << "Failed to parse OpenAI Responses payload";
    return absl::InternalError("Failed to parse OpenAI Responses payload");
  }
  const auto& j = *j_opt;

  const int total_tokens = RecordOpenAiResponsesUsage(db_, session_id, model_, j);

  const auto output = json_get<nlohmann::json::array_t>(j, "output");
  if (!output || output->empty()) {
    return absl::InternalError("OpenAI Responses payload missing output");
  }
  if (IsDebugToolsEnabled()) {
    size_t message_count = 0;
    size_t function_count = 0;
    for (const auto& item : *output) {
      const std::string type = json_get_or(item, "type", std::string{});
      if (type == "message") ++message_count;
      if (type == "function_call") ++function_count;
    }
    LOG(INFO) << "[tool_debug] Responses ProcessResponse model=" << model_ << " output_items=" << output->size()
              << " messages=" << message_count << " function_calls=" << function_count;
  }

  std::string assistant_text;
  nlohmann::json tool_calls = nlohmann::json::array();

  for (const auto& item : *output) {
    const std::string type = json_get_or(item, "type", std::string{});
    if (type == "message") {
      const auto content = json_get<nlohmann::json::array_t>(item, "content");
      if (!content) {
        continue;
      }
      for (const auto& part : *content) {
        const std::string part_type = json_get_or(part, "type", std::string{});
        if (part_type == "output_text") {
          const std::string text = json_get_or(part, "text", std::string{});
          if (!text.empty()) {
            if (!assistant_text.empty()) {
              assistant_text += "\n";
            }
            assistant_text += text;
          }
        }
      }
    } else if (type == "function_call") {
      std::string call_id = json_get_or(item, "call_id", std::string{});
      if (call_id.empty()) {
        call_id = json_get_or(item, "id", std::string{});
      }
      const std::string name = json_get_or(item, "name", std::string{});
      std::string arguments = "{}";
      if (const auto* args_json = json_at(item, "arguments")) {
        if (args_json->is_string()) {
          arguments = json_getter<std::string>::get(*args_json).value_or("{}");
        } else if (args_json->is_object() || args_json->is_array()) {
          arguments = json_dump(*args_json);
        }
      }
      tool_calls.push_back(
          {{"id", call_id}, {"type", "function"}, {"function", {{"name", name}, {"arguments", arguments}}}});
      if (IsDebugToolsEnabled()) {
        LOG(INFO) << "[tool_debug] Responses tool_call name=" << name << " call_id=" << call_id
                  << " args_len=" << arguments.size();
      }
    }
  }

  if (!tool_calls.empty()) {
    if (!assistant_text.empty()) {
      auto text_st = db_->AppendMessage(session_id, "assistant", assistant_text, "", "completed", group_id, GetName(),
                                        total_tokens);
      if (!text_st.ok()) {
        return text_st;
      }
      auto state = Orchestrator::ExtractState(assistant_text);
      if (state) {
        db_->SetSessionState(session_id, *state).IgnoreError();
      }
    }
    nlohmann::json msg = {{"role", "assistant"}, {"tool_calls", tool_calls}};
    const auto& first_call = tool_calls[0];
    const std::string first_id = json_get_or(first_call, "id", std::string{});
    const auto* fn = json_at(first_call, "function");
    const std::string first_name = fn == nullptr ? "" : json_get_or(*fn, "name", std::string{});
    auto st = db_->AppendMessage(session_id, "assistant", json_dump(msg), first_id + "|" + first_name, "tool_call",
                                 group_id, GetName(), assistant_text.empty() ? total_tokens : 0);
    if (!st.ok()) {
      return st;
    }
    return total_tokens;
  }

  if (!assistant_text.empty()) {
    auto st =
        db_->AppendMessage(session_id, "assistant", assistant_text, "", "completed", group_id, GetName(), total_tokens);
    if (!st.ok()) {
      return st;
    }
    auto state = Orchestrator::ExtractState(assistant_text);
    if (state) {
      db_->SetSessionState(session_id, *state).IgnoreError();
    }
    return total_tokens;
  }

  return absl::InternalError("OpenAI Responses output contained no tool calls or assistant text");
}

absl::StatusOr<std::vector<ToolCall>> OpenAiResponsesOrchestrator::ParseToolCalls(const Database::Message& msg) {
  return MessageParser::ExtractToolCalls(MessageContext(msg));
}

absl::StatusOr<std::vector<ModelInfo>> OpenAiResponsesOrchestrator::GetModels(const std::string& api_key,
                                                                              const std::string& account_id) {
  return GetOpenAiModels(http_client_, base_url_, api_key, account_id);
}

absl::StatusOr<nlohmann::json> OpenAiResponsesOrchestrator::GetQuota(const std::string& oauth_token) {
  (void)oauth_token;
  return absl::UnimplementedError("Quota check not implemented for OpenAI Responses strategy");
}

}  // namespace slop

