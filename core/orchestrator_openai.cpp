#include "core/orchestrator_openai.h"

#include <iostream>

#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/substitute.h"

#include "core/message_parser.h"
#include "core/orchestrator.h"
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
      auto j = nlohmann::json::parse(msg.content, nullptr, false);
      if (!j.is_discarded()) {
        bool valid = true;
        if (j.contains("tool_calls")) {
          for (auto& tc : j["tool_calls"]) {
            std::string name = tc["function"]["name"];
            if (!enabled_tool_names.contains(name)) {
              LOG(WARNING) << "Filtering out invalid tool call: " << name;
              valid = false;
              break;
            }
          }
        }
        if (valid) {
          msg_obj = j;
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

    if (!messages.empty() && messages.back()["role"] == msg.role && msg.role == "user") {
      messages.back()["content"] =
          messages.back()["content"].get<std::string>() + "\n" + msg_obj["content"].get<std::string>();
    } else {
      messages.push_back(msg_obj);
    }
  }

  nlohmann::json payload = {{"model", model_}, {"messages", messages}};

  nlohmann::json tools = nlohmann::json::array();
  if (tools_or.ok()) {
    for (const auto& t : *tools_or) {
      auto schema = nlohmann::json::parse(t.json_schema, nullptr, false);
      if (!schema.is_discarded()) {
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
  auto j = nlohmann::json::parse(response_json, nullptr, false);
  if (j.is_discarded()) {
    LOG(ERROR) << "Failed to parse OpenAI response: " << response_json;
    return absl::InternalError("Failed to parse LLM response");
  }

  int total_tokens = 0;
  if (j.contains("usage")) {
    auto& usage = j["usage"];
    int prompt = usage.value("prompt_tokens", 0);
    int completion = usage.value("completion_tokens", 0);
    total_tokens = prompt + completion;
    (void)db_->RecordUsage(session_id, model_, prompt, completion);
  }

  absl::Status status = absl::InternalError("No choices in response");
  if (j.contains("choices") && !j["choices"].empty()) {
    if (!j["choices"][0].contains("message")) {
      return absl::InternalError("OpenAI response choice missing 'message'");
    }
    auto& msg = j["choices"][0]["message"];
    if (msg.contains("tool_calls") && !msg["tool_calls"].empty()) {
      status = db_->AppendMessage(session_id, "assistant",
                                  msg.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),
                                  msg["tool_calls"][0]["id"].get<std::string>() + "|" +
                                      msg["tool_calls"][0]["function"]["name"].get<std::string>(),
                                  "tool_call", group_id, GetName(), total_tokens);
    } else if (msg.contains("content") && !msg["content"].is_null()) {
      std::string text = msg["content"];
      status = db_->AppendMessage(session_id, "assistant", text, "", "completed", group_id, GetName(), total_tokens);

      auto state = Orchestrator::ExtractState(text);
      if (state) {
        db_->SetSessionState(session_id, *state).IgnoreError();
      }
    }
  }
  if (!status.ok()) return status;
  return total_tokens;
}

absl::StatusOr<std::vector<ToolCall>> OpenAiOrchestrator::ParseToolCalls(const Database::Message& msg) {
  return MessageParser::ExtractToolCalls(msg);
}

absl::StatusOr<std::vector<ModelInfo>> OpenAiOrchestrator::GetModels(const std::string& api_key) {
  std::vector<std::string> headers = {"Authorization: Bearer " + api_key};
  std::string url = base_url_ + "/models";
  auto resp_or = http_client_->Get(url, headers);
  if (!resp_or.ok()) return resp_or.status();

  auto j = nlohmann::json::parse(*resp_or, nullptr, false);
  if (j.is_discarded()) return absl::InternalError("Failed to parse models response");

  std::vector<ModelInfo> models;
  if (j.contains("data")) {
    for (const auto& m : j["data"]) {
      ModelInfo info;
      info.id = m["id"];
      info.name = m["id"];
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
