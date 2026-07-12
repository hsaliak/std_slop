#include "core/orchestrator_openai.h"

#include <iostream>

#include "absl/container/flat_hash_set.h"
#include "absl/log/log.h"
#include "absl/strings/str_cat.h"

#include "core/message_parser.h"
#include "core/openai_utils.h"
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

  const absl::flat_hash_set<std::string> enabled_tool_names = GetEnabledToolNames(db_);

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

  nlohmann::json tools = BuildOpenAiChatTools(db_);
  if (!tools.empty()) payload["tools"] = tools;

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

  int total_tokens = RecordOpenAiChatUsage(db_, session_id, model_, j);

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

      }

      // Store reasoning as separate assistant message in DB
      auto reasoning = json_get<std::string>(*msg, "reasoning_content");
      if (reasoning && !reasoning->empty()) {
        db_->AppendMessage(session_id, "assistant", absl::StrCat("🤔 **Reasoning:**\n", *reasoning), "", "completed",
                           group_id, GetName(), 0)
            .IgnoreError();
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

absl::StatusOr<std::string> OpenAiOrchestrator::ExtractAssistantText(const std::string& response_body) {
  auto j_opt = json_parse(response_body);
  if (!j_opt) {
    return absl::InternalError("Failed to parse LLM response");
  }

  const auto choices = json_get<nlohmann::json::array_t>(*j_opt, "choices");
  if (!choices || choices->empty()) {
    return absl::InternalError("OpenAI response missing choices");
  }
  const auto* msg = json_at((*choices)[0], "message");
  if (msg == nullptr) {
    return absl::InternalError("OpenAI response choice missing message");
  }
  auto content = json_get<std::string>(*msg, "content");
  if (!content || content->empty()) {
    return absl::InternalError("OpenAI response message missing assistant text");
  }
  return *content;
}

absl::StatusOr<std::vector<ModelInfo>> OpenAiOrchestrator::GetModels(const std::string& api_key,
                                                                     const std::string& account_id) {
  return GetOpenAiModels(http_client_, base_url_, api_key, account_id);
}

absl::StatusOr<nlohmann::json> OpenAiOrchestrator::GetQuota(const std::string& oauth_token) {
  (void)oauth_token;
  return absl::UnimplementedError("Quota check not implemented for OpenAI Strategy yet");
}

}  // namespace slop
