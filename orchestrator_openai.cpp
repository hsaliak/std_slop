#include "orchestrator.h"
#include "orchestrator_openai.h"

#include <iostream>

#include "absl/strings/substitute.h"
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

  for (size_t i = 0; i < history.size(); ++i) {
    const auto& msg = history[i];
    std::string display_content = msg.content;

    if (i == 0) display_content = "--- BEGIN CONVERSATION HISTORY ---\n" + display_content;
    if (i == history.size() - 1 && msg.role == "user" && i > 0) {
      display_content = "--- END OF HISTORY ---\n\n### CURRENT REQUEST\n" + display_content;
    }

    if (msg.role == "system") continue;

    nlohmann::json msg_obj;

    if (msg.status == "tool_call") {
      auto j = nlohmann::json::parse(msg.content, nullptr, false);
      if (!j.is_discarded()) {
        msg_obj = j;
      } else {
        msg_obj = {{"role", msg.role}, {"content", display_content}};
      }
    } else if (msg.role == "tool") {
      msg_obj = {{"role", msg.role}};
      msg_obj["tool_call_id"] = msg.tool_call_id.substr(0, msg.tool_call_id.find('|'));
      msg_obj["content"] = SmarterTruncate(msg.content, kMaxToolResultContext);
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
  auto tools_or = db_->GetEnabledTools();
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

absl::Status OpenAiOrchestrator::ProcessResponse(const std::string& session_id, const std::string& response_json,
                                                 const std::string& group_id) {
  auto j = nlohmann::json::parse(response_json, nullptr, false);
  if (j.is_discarded()) return absl::InternalError("Failed to parse LLM response");

  if (j.contains("usage")) {
    auto& usage = j["usage"];
    int prompt = usage.value("prompt_tokens", 0);
    int completion = usage.value("completion_tokens", 0);
    (void)db_->RecordUsage(session_id, model_, prompt, completion);
  }

  absl::Status status = absl::InternalError("No choices in response");
  if (j.contains("choices") && !j["choices"].empty()) {
    auto& msg = j["choices"][0]["message"];
    if (msg.contains("tool_calls") && !msg["tool_calls"].empty()) {
      status = db_->AppendMessage(session_id, "assistant", msg.dump(),
                                  msg["tool_calls"][0]["id"].get<std::string>() + "|" +
                                      msg["tool_calls"][0]["function"]["name"].get<std::string>(),
                                  "tool_call", group_id, GetName());
    } else if (msg.contains("content") && !msg["content"].is_null()) {
      std::string text = msg["content"];
      status = db_->AppendMessage(session_id, "assistant", text, "", "completed", group_id, GetName());

      size_t start_pos = text.find("---STATE---");
      if (start_pos != std::string::npos) {
        size_t end_pos = text.find("---END STATE---", start_pos);
        std::string state_blob;
        if (end_pos != std::string::npos) {
          state_blob = text.substr(start_pos, end_pos - start_pos + 15);
        } else {
          state_blob = text.substr(start_pos);
        }
        (void)db_->SetSessionState(session_id, state_blob);
      }
    }
  }
  return status;
}

absl::StatusOr<std::vector<ToolCall>> OpenAiOrchestrator::ParseToolCalls(const Database::Message& msg) {
  if (msg.status != "tool_call") return absl::InvalidArgumentError("Not a tool call");
  auto j = nlohmann::json::parse(msg.content, nullptr, false);
  if (j.is_discarded()) return absl::InternalError("JSON error");

  std::vector<ToolCall> calls;
  if (j.contains("tool_calls")) {
    for (const auto& call : j["tool_calls"]) {
      ToolCall tc;
      tc.name = call["function"]["name"];
      tc.args = nlohmann::json::parse(call["function"]["arguments"].get<std::string>(), nullptr, false);
      calls.push_back(tc);
    }
  }
  return calls;
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

int OpenAiOrchestrator::CountTokens(const nlohmann::json& prompt) { return prompt.dump().length() / 4; }

std::string OpenAiOrchestrator::SmarterTruncate(const std::string& content, size_t limit) {
  if (content.size() <= limit) return content;
  std::string truncated = content.substr(0, limit);
  std::string metadata = absl::Substitute(
      "\n... [TRUNCATED: Showing $0/$1 characters. Use the tool again with an offset to read more.] ...", limit,
      content.size());
  return truncated + metadata;
}

}  // namespace slop
