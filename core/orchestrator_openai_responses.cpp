#include "core/orchestrator_openai_responses.h"

#include <cstdlib>
#include <unordered_map>

#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"

#include "absl/container/flat_hash_set.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"

#include "core/message_parser.h"
#include "core/openai_utils.h"
#include "core/orchestrator.h"
#include "core/sha256.h"
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

constexpr absl::string_view kPromptCacheKeyPrefix = "slop:";
constexpr size_t kPromptCacheKeyMaxLength = 64;

std::optional<nlohmann::json> TryNormalizeSseResponsesPayload(const std::string& payload) {
  if (!absl::StrContains(payload, "event:") || !absl::StrContains(payload, "data:")) {
    return std::nullopt;
  }

  nlohmann::json output = nlohmann::json::array();
  std::unordered_map<std::string, size_t> output_index_by_key;
  nlohmann::json usage;
  bool saw_stream_event = false;
  std::string output_text_delta;
  std::string output_text_done;
  bool saw_output_text_item = false;
  absl::flat_hash_set<std::string> seen_message_texts;

  const auto upsert_output_item = [&](const nlohmann::json& item) {
    const std::string type = json_get_or(item, "type", std::string{});
    if (type == "message") {
      const auto* content = json_at(item, "content");
      if (content != nullptr && content->is_array()) {
        for (const auto& part : *content) {
          if (json_get_or(part, "type", std::string{}) == "output_text" &&
              !json_get_or(part, "text", std::string{}).empty()) {
            saw_output_text_item = true;
            break;
          }
        }
      }
    }
    std::string stable_id =
        type == "function_call" ? json_get_or(item, "call_id", std::string{}) : std::string{};
    if (stable_id.empty()) stable_id = json_get_or(item, "id", std::string{});
    const std::string key = stable_id.empty() ? std::string{} : absl::StrCat(type, ":", stable_id);
    const auto it = output_index_by_key.find(key);
    if (!key.empty() && it == output_index_by_key.end()) {
      output_index_by_key.emplace(key, output.size());
      output.push_back(item);
      return;
    }
    if (!key.empty() && type != "function_call") {
      output[it->second] = item;
      return;
    }

    if (!key.empty()) {
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
      return;
    }

    const std::string text_key = json_dump(item);
    if (type == "message" && !seen_message_texts.insert(text_key).second) {
      return;
    }
    output.push_back(item);
  };

  const auto merge_response_output = [&](const nlohmann::json& response) {
    const auto response_output = json_get<nlohmann::json::array_t>(response, "output");
    if (!response_output) {
      return;
    }
    for (const auto& item : *response_output) {
      upsert_output_item(item);
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
    if (type == "response.output_text.done") {
      output_text_done = json_get_or(evt, "text", std::string{});
      return;
    }
    if (type == "response.completed" || type == "response.done") {
      const auto* response = json_at(evt, "response");
      if (response != nullptr) {
        const auto* usage_obj = json_at(*response, "usage");
        if (usage_obj != nullptr) {
          usage = *usage_obj;
        }
        merge_response_output(*response);
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
  const std::string fallback_text = !output_text_done.empty() ? output_text_done : output_text_delta;
  if (!fallback_text.empty() && !saw_output_text_item) {
    output.push_back({{"type", "message"},
                      {"role", "assistant"},
                      {"content", nlohmann::json::array({{{"type", "output_text"}, {"text", fallback_text}}})}});
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

absl::StatusOr<nlohmann::json> OpenAiResponsesOrchestrator::BuildRequest(const ResponsesRequestInput& request) {
  std::string system_instruction = request.system_instruction;
  if (!request.active_skill_content.empty()) {
    if (!system_instruction.empty()) {
      system_instruction.append("\n\n");
    }
    system_instruction.append(request.active_skill_content);
  }
  const std::vector<Database::Message>& history = request.history;
  const auto model_selection_or = ParseResponsesModelSelection(model_);
  if (!model_selection_or.ok()) {
    return model_selection_or.status();
  }
  const ModelSelection& model_selection = *model_selection_or;

  nlohmann::json input = nlohmann::json::array();

  const auto enabled_tool_names = GetEnabledToolNames(request.enabled_tools);

  for (const auto& msg : history) {
    if (msg.role == "system") {
      continue;
    }

    if (!msg.api_item_json.empty()) {
      auto item = json_parse(msg.api_item_json);
      if (!item || !item->is_object()) {
        return absl::InvalidArgumentError("Stored Responses API item is not a valid JSON object");
      }
      const std::string type = json_get_or(*item, "type", std::string{});
      if (type == "function_call") {
        const std::string name = json_get_or(*item, "name", std::string{});
        if (!enabled_tool_names.contains(name)) {
          LOG(WARNING) << "Filtering invalid tool call from history: " << name;
          continue;
        }
      } else if (type == "function_call_output") {
        const std::string tool_name =
            absl::StrContains(msg.tool_call_id, '|') ? msg.tool_call_id.substr(msg.tool_call_id.find('|') + 1) : "";
        if (!tool_name.empty() && !enabled_tool_names.contains(tool_name)) {
          LOG(WARNING) << "Filtering invalid tool response from history: " << tool_name;
          continue;
        }
      }
      input.push_back(std::move(*item));
      continue;
    }

    if (msg.status == "tool_call") {
      auto j_opt = json_parse(msg.content);
      if (!j_opt) {
        continue;
      }
      if (json_get_or(*j_opt, "type", std::string{}) == "function_call") {
        const std::string name = json_get_or(*j_opt, "name", std::string{});
        if (enabled_tool_names.contains(name)) {
          input.push_back(std::move(*j_opt));
        }
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

    input.push_back({{"role", msg.role}, {"content", msg.content}});
  }

  nlohmann::json payload = {{"model", model_selection.base_model}, {"input", input}, {"store", false}};
  if (!system_instruction.empty()) {
    payload["instructions"] = system_instruction;
  }
  // These are standard Responses API options for every compatible endpoint.
  payload["stream"] = true;
  payload["reasoning"] = {{"effort", model_selection.reasoning_effort}, {"summary", "auto"}};
  nlohmann::json tools = BuildOpenAiResponsesTools(request.enabled_tools);
  if (request.structured_output_schema.has_value()) {
    payload["instructions"] = absl::StrCat(
        json_get_or(payload, "instructions", std::string{}),
        "\n\n## Structured output\nUse normal tools if needed, then call structured_output exactly once with the "
        "final JSON result. Do not emit assistant text in structured-output mode.");
    tools.push_back(BuildStructuredOutputTool(*request.structured_output_schema));
  }
  if (!tools.empty()) {
    payload["tools"] = tools;
  }
  // Both OpenAI and OpenRouter treat this as a routing hint. Session identity
  // keeps a conversation on the same cache shard without provider extensions.
  if (!request.session_id.empty()) {
    auto digest_or = Sha256Digest(request.session_id);
    if (digest_or.ok()) {
      std::string digest_hex = absl::BytesToHexString(absl::string_view(
          reinterpret_cast<const char*>(digest_or->data()), digest_or->size()));
      digest_hex.resize(kPromptCacheKeyMaxLength - kPromptCacheKeyPrefix.size());
      payload["prompt_cache_key"] = absl::StrCat(kPromptCacheKeyPrefix, digest_hex);
    } else {
      LOG(WARNING) << "Failed to compute prompt_cache_key: " << digest_or.status();
    }
  }
  if (IsDebugToolsEnabled()) {
    const size_t input_count = input.is_array() ? input.size() : 0;
    const size_t tools_count = tools.is_array() ? tools.size() : 0;
    LOG(INFO) << "[tool_debug] Responses BuildRequest model=" << model_
              << " input_items=" << input_count << " tools=" << tools_count
              << " stream=" << json_get_or(payload, "stream", false);
  }
  return payload;
}

absl::StatusOr<int> OpenAiResponsesOrchestrator::ProcessResponse(const std::string& session_id,
                                                                 const std::string& response_json,
                                                                 const std::string& group_id) {
  last_response_usage_.reset();
  last_output_items_.clear();
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
  last_response_usage_ = ParseOpenAiResponsesUsage(j);

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

  for (const auto& item : *output) {
    if (!item.is_object()) {
      return absl::InvalidArgumentError("OpenAI Responses output item must be an object");
    }
    ResponsesOutputItem output_item;
    output_item.id = json_get_or(item, "id", std::string{});
    output_item.type = json_get_or(item, "type", std::string{});
    output_item.status = json_get_or(item, "status", std::string{});
    output_item.raw = item;
    last_output_items_.push_back(std::move(output_item));
  }

  bool has_function_calls = false;
  std::string assistant_text;
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
      has_function_calls = true;
    }
  }

  if (!has_function_calls && assistant_text.empty()) {
    return absl::InternalError("OpenAI Responses output contained no tool calls or assistant text");
  }
  if (has_function_calls) {
    auto calls_or = ParseLastOutputToolCalls();
    if (!calls_or.ok()) return calls_or.status();
  }

  bool assigned_tokens = false;
  for (const auto& item : *output) {
    const std::string type = json_get_or(item, "type", std::string{});
    std::string content;
    std::string tool_call_id;
    std::string status = "provider_item";

    if (type == "reasoning") {
      status = "reasoning";
    } else if (type == "message") {
      const auto parts = json_get<nlohmann::json::array_t>(item, "content");
      if (parts) {
        for (const auto& part : *parts) {
          if (json_get_or(part, "type", std::string{}) != "output_text") continue;
          const std::string text = json_get_or(part, "text", std::string{});
          if (!text.empty()) {
            if (!content.empty()) content.push_back('\n');
            content.append(text);
          }
        }
      }
      status = has_function_calls ? "intermediate" : "completed";
    } else if (type == "function_call") {
      std::string call_id = json_get_or(item, "call_id", std::string{});
      if (call_id.empty()) call_id = json_get_or(item, "id", std::string{});
      const std::string name = json_get_or(item, "name", std::string{});
      std::string arguments = "{}";
      if (const auto* args_json = json_at(item, "arguments")) {
        if (args_json->is_string()) {
          arguments = json_getter<std::string>::get(*args_json).value_or("{}");
        } else if (args_json->is_object() || args_json->is_array()) {
          arguments = json_dump(*args_json);
        }
      }
      const nlohmann::json projected_call = {
          {"id", call_id}, {"type", "function"}, {"function", {{"name", name}, {"arguments", arguments}}}};
      content = json_dump({{"role", "assistant"}, {"tool_calls", nlohmann::json::array({projected_call})}});
      tool_call_id = call_id + "|" + name;
      status = "tool_call";
      if (IsDebugToolsEnabled()) {
        LOG(INFO) << "[tool_debug] Responses tool_call name=" << name << " call_id=" << call_id
                  << " args_len=" << arguments.size();
      }
    }

    const bool should_assign_tokens =
        !assigned_tokens && (status == "tool_call" || (!has_function_calls && status == "completed"));
    auto st = db_->AppendMessage(session_id, "assistant", content, tool_call_id, status, group_id, "openai",
                                 should_assign_tokens ? total_tokens : 0, json_dump(item));
    if (!st.ok()) {
      return st;
    }
    assigned_tokens = assigned_tokens || should_assign_tokens;
  }

  return total_tokens;
}

absl::StatusOr<std::vector<ToolCall>> OpenAiResponsesOrchestrator::ParseLastOutputToolCalls() const {
  std::vector<ToolCall> calls;
  for (const ResponsesOutputItem& item : last_output_items_) {
    if (item.type != "function_call") continue;
    ToolCall call;
    call.id = json_get_or(item.raw, "call_id", item.id);
    call.name = json_get_or(item.raw, "name", std::string{});
    if (call.id.empty() || call.name.empty()) {
      return absl::InvalidArgumentError("Responses function call is missing call_id or name");
    }
    const auto* arguments = json_at(item.raw, "arguments");
    if (arguments == nullptr) {
      return absl::InvalidArgumentError("Responses function call is missing arguments");
    }
    if (arguments->is_string()) {
      auto args_or = json_parse(json_get_or(item.raw, "arguments", std::string{}));
      if (!args_or) return absl::InvalidArgumentError("Responses function call arguments are invalid JSON");
      call.args = *args_or;
    } else if (arguments->is_object() || arguments->is_array()) {
      call.args = *arguments;
    } else {
      return absl::InvalidArgumentError("Responses function call arguments must be JSON");
    }
    calls.push_back(std::move(call));
  }
  return calls;
}

absl::StatusOr<std::vector<ToolCall>> OpenAiResponsesOrchestrator::ParseToolCalls(const Database::Message& msg) {
  return MessageParser::ExtractToolCalls(MessageContext(msg));
}

absl::StatusOr<std::string> OpenAiResponsesOrchestrator::ExtractAssistantText(const std::string& response_body) {
  auto j_opt = json_parse(response_body);
  if (!j_opt) {
    auto sse_normalized = TryNormalizeSseResponsesPayload(response_body);
    if (sse_normalized.has_value()) {
      j_opt = sse_normalized;
    }
  }
  if (!j_opt) {
    return absl::InternalError("Failed to parse LLM response");
  }

  const auto output = json_get<nlohmann::json::array_t>(*j_opt, "output");
  if (!output || output->empty()) {
    return absl::InternalError("OpenAI Responses payload missing output");
  }

  std::string assistant_text;
  for (const auto& item : *output) {
    if (json_get_or(item, "type", std::string{}) != "message") {
      continue;
    }
    const auto content = json_get<nlohmann::json::array_t>(item, "content");
    if (!content) {
      continue;
    }
    for (const auto& part : *content) {
      if (json_get_or(part, "type", std::string{}) != "output_text") {
        continue;
      }
      const std::string text = json_get_or(part, "text", std::string{});
      if (text.empty()) {
        continue;
      }
      if (!assistant_text.empty()) {
        assistant_text.push_back('\n');
      }
      assistant_text.append(text);
    }
  }
  if (assistant_text.empty()) {
    return absl::InternalError("OpenAI Responses output contained no assistant text");
  }
  return assistant_text;
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
