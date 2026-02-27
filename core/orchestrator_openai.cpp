#include "core/orchestrator_openai.h"
#include "core/constants.h"

#include <iostream>

#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/substitute.h"

#include "core/message_parser.h"
#include "core/orchestrator.h"
#include "json_utils.h"
namespace slop {

OpenAiOrchestrator::OpenAiOrchestrator(Database* db, HttpClient* http_client, const std::string& model,
                                       const std::string& base_url)
    : db_(db), http_client_(http_client), model_(model), base_url_(base_url) {}

absl::StatusOr<nlohmann::json> OpenAiOrchestrator::AssemblePayload(const std::string& session_id,
                                                                   const std::string& system_instruction,
                                                                   const std::vector<Database::Message>& history) {
  (void)session_id;
  nlohmann::json messages = nlohmann::json::array();
  if (!system_instruction.empty()) messages.push_back({{"role", "system"}, {"content", system_instruction}});

  absl::flat_hash_set<std::string> enabled_tool_names;
  auto tools_or = db_->GetEnabledTools();
  if (tools_or.ok()) {
    for (const auto& t : *tools_or) {
      enabled_tool_names.insert(t.name);
    }
  }

  for (size_t i = 0; i < history.size(); ++i) {
    const auto& msg = history[i];
    std::string display_content = msg.content;

    if (i == 0) display_content = "## Begin Conversation History\n" + display_content;
    if (i == history.size() - 1 && msg.role == "user" && i > 0) {
      display_content = "## End of History\n\n### CURRENT REQUEST\n" + display_content;
    }

    if (msg.role == "system") continue;

    nlohmann::json msg_obj;

    if (msg.status == "tool_call") {
      auto j_opt = json_parse(msg.content);
      if (j_opt) {
        auto& j = *j_opt;
        bool valid = true;
        if (auto tool_calls = json_get<nlohmann::json::array_t>(j, "tool_calls")) {
          for (auto& tc : *tool_calls) {
            if (const auto* fn = json_at(tc, "function")) {
              std::string name = json_get_or(*fn, "name", std::string{});
              if (!enabled_tool_names.contains(name)) {
                LOG(WARNING) << "Filtering out invalid tool call: " << name;
                valid = false;
                break;
              }
            }
          }
        }
        if (valid) {
          msg_obj = j;
          if (json_get_or(msg_obj, "role", std::string{}).empty()) {
            msg_obj["role"] = "assistant";
          }
        } else {
          msg_obj = {{"role", "assistant"}, {"content", "[Invalid tool call suppressed]"}};
        }
      } else {
        msg_obj = {{"role", msg.role}, {"content", display_content}};
      }
    } else if (msg.role == "tool") {
      bool valid = true;
      std::string name = msg.tool_call_id.substr(msg.tool_call_id.find('|') + 1);
      if (!enabled_tool_names.contains(name)) {
        LOG(WARNING) << "Filtering out invalid tool response: " << name;
        valid = false;
      }

      if (valid) {
        msg_obj = {{"role", msg.role}};
        msg_obj["tool_call_id"] = msg.tool_call_id.substr(0, msg.tool_call_id.find('|'));
        msg_obj["content"] = msg.content;
      } else {
        msg_obj = {{"role", "user"}, {"content", "[Invalid tool response suppressed]"}};
      }
    } else {
      msg_obj = {{"role", msg.role}, {"content", display_content}};
    }

    if (!messages.empty() && json_get_or(messages.back(), "role", std::string{}) == msg.role && msg.role == "user") {
      messages.back()["content"] = json_get_or(messages.back(), "content", std::string{}) + "\n" +
                                   json_get_or(msg_obj, "content", std::string{});
    } else {
      messages.push_back(msg_obj);
    }
  }

  nlohmann::json payload = {{"model", model_}, {"messages", messages}};

  nlohmann::json tools = nlohmann::json::array();
  if (tools_or.ok()) {
    for (const auto& t : *tools_or) {
      auto schema_opt = json_parse(t.json_schema);
      if (schema_opt) {
        auto& schema = *schema_opt;
        tools.push_back({{"type", "function"},
                         {"function", {{"name", t.name}, {"description", t.description}, {"parameters", schema}}}});
      }
    }
  }
  if (!tools.empty()) payload["tools"] = tools;

  if (strip_reasoning_) {
    payload["transforms"] = {"strip_reasoning"};
  }

  return payload;
}

absl::StatusOr<int> OpenAiOrchestrator::ProcessResponse(const std::string& session_id, const std::string& response_json,
                                                        const std::string& group_id) {
  auto j_opt = json_parse(response_json);
  if (!j_opt) {
    LOG(ERROR) << "Failed to parse OpenAI response: " << response_json;
    return absl::InternalError("Failed to parse LLM response");
  }
  auto& j = *j_opt;

  int total_tokens = 0;
  if (const auto* usage = json_at(j, "usage")) {
    int prompt = json_get_or(*usage, "prompt_tokens", 0);
    int completion = json_get_or(*usage, "completion_tokens", 0);
    total_tokens = prompt + completion;
    (void)db_->RecordUsage(session_id, model_, prompt, completion);
  }

  absl::Status status = absl::InternalError("No choices in response");
  if (auto choices = json_get<nlohmann::json::array_t>(j, "choices"); choices && !choices->empty()) {
    auto& choice = (*choices)[0];
    if (const auto* msg = json_at(choice, "message")) {
      if (auto tool_calls = json_get<nlohmann::json::array_t>(*msg, "tool_calls"); tool_calls && !tool_calls->empty()) {
        auto& first_call = (*tool_calls)[0];
        std::string call_id = json_get_or(first_call, "id", std::string{});
        std::string fn_name;
        if (const auto* fn = json_at(first_call, "function")) {
          fn_name = json_get_or(*fn, "name", std::string{});
        }
        status = db_->AppendMessage(session_id, "assistant", json_dump(*msg), call_id + "|" + fn_name, "tool_call",
                                    group_id, GetName(), total_tokens);
      } else if (auto content = json_get<std::string>(*msg, "content")) {
        status =
            db_->AppendMessage(session_id, "assistant", *content, "", "completed", group_id, GetName(), total_tokens);

        auto state = Orchestrator::ExtractState(*content);
        if (state) {
          db_->SetSessionState(session_id, *state).IgnoreError();
        }
      }
    } else {
      return absl::InternalError("OpenAI response choice missing 'message'");
    }
  }
  if (!status.ok()) return status;
  return total_tokens;
}

absl::StatusOr<std::vector<ToolCall>> OpenAiOrchestrator::ParseToolCalls(const Database::Message& msg) {
  return MessageParser::ExtractToolCalls(MessageContext(msg));
}

absl::StatusOr<std::vector<ModelInfo>> OpenAiOrchestrator::GetModels(const std::string& api_key) {
  std::vector<std::string> headers = {"Authorization: Bearer " + api_key};
  headers.push_back(std::string("User-Agent: ") + kUserAgent);

  std::string url = base_url_ + "/models";
  auto resp_or = http_client_->Get(url, headers);
  if (!resp_or.ok()) return resp_or.status();

  auto j_opt = json_parse(*resp_or);
  if (!j_opt) return absl::InternalError("Failed to parse models response");
  auto& j = *j_opt;

  std::vector<ModelInfo> models;
  if (auto data = json_get<nlohmann::json::array_t>(j, "data")) {
    for (const auto& m : *data) {
      ModelInfo info;
      info.id = json_get_or(m, "id", std::string{});
      info.name = json_get_or(m, "id", std::string{});
      models.push_back(info);
    }
  }

  return models;
}

absl::StatusOr<nlohmann::json> OpenAiOrchestrator::GetQuota(const std::string& oauth_token) {
  (void)oauth_token;
  return absl::UnimplementedError("Quota check not implemented for OpenAI Strategy yet");
}

}  // namespace slop
