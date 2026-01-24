#include "orchestrator.h"
#include "orchestrator_gemini.h"

#include <iostream>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/substitute.h"
#include "absl/time/clock.h"
namespace slop {

GeminiOrchestrator::GeminiOrchestrator(Database* db, HttpClient* http_client, const std::string& model,
                                       const std::string& base_url)
    : db_(db), http_client_(http_client), model_(model), base_url_(base_url) {}

absl::StatusOr<nlohmann::json> GeminiOrchestrator::AssemblePayload(const std::string& session_id,
                                                                   const std::string& system_instruction,
                                                                   const std::vector<Database::Message>& history) {
  (void)session_id;
  nlohmann::json payload;
  nlohmann::json contents = nlohmann::json::array();

  for (size_t i = 0; i < history.size(); ++i) {
    const auto& msg = history[i];
    std::string display_content = msg.content;

    if (i == 0) display_content = "--- BEGIN CONVERSATION HISTORY ---\n" + display_content;
    if (i == history.size() - 1 && msg.role == "user" && i > 0) {
      display_content = "--- END OF HISTORY ---\n\n### CURRENT REQUEST\n" + display_content;
    }

    if (msg.role == "system") continue;

    std::string role = (msg.role == "assistant") ? "model" : (msg.role == "tool" ? "function" : msg.role);
    nlohmann::json part;

    if (msg.status == "tool_call") {
      auto j = nlohmann::json::parse(msg.content, nullptr, false);
      if (!j.is_discarded()) {
        part = j;
      } else {
        part = {{"text", display_content}};
      }
    } else if (msg.role == "tool") {
      part = {{"functionResponse",
               {{"name", msg.tool_call_id.substr(msg.tool_call_id.find('|') + 1)},
                {"response", {{"content", SmarterTruncate(msg.content, kMaxToolResultContext)}}}}}};
    } else {
      part = {{"text", display_content}};
    }

    if (!contents.empty() && contents.back()["role"] == role)
      contents.back()["parts"].push_back(part);
    else
      contents.push_back({{"role", role}, {"parts", {part}}});
  }

  nlohmann::json valid_contents = nlohmann::json::array();
  for (const auto& c : contents) {
    if (c["role"] == "function" && (valid_contents.empty() || valid_contents.back()["role"] != "model")) continue;
    valid_contents.push_back(c);
  }

  payload["contents"] = valid_contents;
  if (!system_instruction.empty()) payload["system_instruction"] = {{"parts", {{{"text", system_instruction}}}}};

  nlohmann::json f_decls = nlohmann::json::array();
  auto tools_or = db_->GetEnabledTools();
  if (tools_or.ok()) {
    for (const auto& t : *tools_or) {
      auto schema = nlohmann::json::parse(t.json_schema, nullptr, false);
      if (!schema.is_discarded())
        f_decls.push_back({{"name", t.name}, {"description", t.description}, {"parameters", schema}});
    }
  }
  if (!f_decls.empty()) payload["tools"] = {{{"function_declarations", f_decls}}};

  return payload;
}

absl::Status GeminiOrchestrator::ProcessResponse(const std::string& session_id, const std::string& response_json,
                                                 const std::string& group_id) {
  auto j = nlohmann::json::parse(response_json, nullptr, false);
  if (j.is_discarded()) {
    LOG(ERROR) << "Failed to parse Gemini response: " << response_json;
    return absl::InternalError("Failed to parse LLM response");
  }

  nlohmann::json* target = &j;
  if (j.contains("response") && j["response"].is_object()) {
    target = &j["response"];
  }

  if (target->contains("usageMetadata")) {
    auto& usage = (*target)["usageMetadata"];
    int prompt = usage.value("promptTokenCount", 0);
    int completion = usage.value("candidatesTokenCount", 0);
    (void)db_->RecordUsage(session_id, model_, prompt, completion);
  }

  absl::Status status = absl::InternalError("No candidates in response");
  if (target->contains("candidates") && !(*target)["candidates"].empty()) {
    CHECK((*target)["candidates"][0].contains("content"));
    auto& parts = (*target)["candidates"][0]["content"]["parts"];
    for (const auto& part : parts) {
      if (part.contains("functionCall")) {
        status = db_->AppendMessage(session_id, "assistant", part.dump(), part["functionCall"]["name"], "tool_call",
                                    group_id, GetName());
      } else if (part.contains("text")) {
        std::string text = part["text"];
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
  }
  return status;
}

absl::StatusOr<std::vector<ToolCall>> GeminiOrchestrator::ParseToolCalls(const Database::Message& msg) {
  if (msg.status != "tool_call") return absl::InvalidArgumentError("Not a tool call");
  auto j = nlohmann::json::parse(msg.content, nullptr, false);
  if (j.is_discarded()) return absl::InternalError("JSON error");

  std::vector<ToolCall> calls;
  ToolCall tc;
  tc.id = msg.tool_call_id;
  tc.name = msg.tool_call_id;
  tc.args = (j.contains("functionCall") && j["functionCall"].contains("args"))
                ? j["functionCall"]["args"]
                : (j.contains("args") ? j["args"] : nlohmann::json::object());
  calls.push_back(tc);
  return calls;
}

absl::StatusOr<std::vector<ModelInfo>> GeminiOrchestrator::GetModels(const std::string& api_key) {
  std::string url = base_url_ + "/models?key=" + api_key;
  auto resp_or = http_client_->Get(url, {});
  if (!resp_or.ok()) return resp_or.status();

  auto j = nlohmann::json::parse(*resp_or, nullptr, false);
  if (j.is_discarded()) return absl::InternalError("Failed to parse models response");

  std::vector<ModelInfo> models;
  if (j.contains("models")) {
    for (const auto& m : j["models"]) {
      ModelInfo info;
      info.id = m["name"];
      info.name = m["displayName"];
      models.push_back(info);
    }
  }
  return models;
}

absl::StatusOr<nlohmann::json> GeminiOrchestrator::GetQuota(const std::string& oauth_token) {
  (void)oauth_token;
  return absl::UnimplementedError("Quota check not implemented for Gemini Strategy yet");
}

int GeminiOrchestrator::CountTokens(const nlohmann::json& prompt) { return prompt.dump().length() / 4; }

std::string GeminiOrchestrator::SmarterTruncate(const std::string& content, size_t limit) {
  if (content.size() <= limit) return content;
  std::string truncated = content.substr(0, limit);
  std::string metadata = absl::Substitute(
      "\n... [TRUNCATED: Showing $0/$1 characters. Use the tool again with an offset to read more.] ...", limit,
      content.size());
  return truncated + metadata;
}

GeminiGcaOrchestrator::GeminiGcaOrchestrator(Database* db, HttpClient* http_client, const std::string& model,
                                             const std::string& base_url, const std::string& project_id)
    : GeminiOrchestrator(db, http_client, model, base_url), project_id_(project_id) {}

absl::StatusOr<nlohmann::json> GeminiGcaOrchestrator::AssemblePayload(const std::string& session_id,
                                                                      const std::string& system_instruction,
                                                                      const std::vector<Database::Message>& history) {
  auto payload_or = GeminiOrchestrator::AssemblePayload(session_id, system_instruction, history);
  if (!payload_or.ok()) return payload_or.status();

  nlohmann::json wrapped;
  wrapped["model"] = model_;
  wrapped["project"] = project_id_;
  wrapped["user_prompt_id"] = std::to_string(absl::ToUnixNanos(absl::Now()));
  nlohmann::json inner_request = *payload_or;
  inner_request["session_id"] = session_id;
  wrapped["request"] = inner_request;
  return wrapped;
}

absl::Status GeminiGcaOrchestrator::ProcessResponse(const std::string& session_id, const std::string& response_json,
                                                    const std::string& group_id) {
  return GeminiOrchestrator::ProcessResponse(session_id, response_json, group_id);
}

absl::StatusOr<std::vector<ModelInfo>> GeminiGcaOrchestrator::GetModels([[maybe_unused]] const std::string& api_key) {
  return absl::UnimplementedError("Model listing not implemented for Gemini OAuth logins yet");
}

absl::StatusOr<nlohmann::json> GeminiGcaOrchestrator::GetQuota(const std::string& oauth_token) {
  if (project_id_.empty()) {
    return absl::FailedPreconditionError("Project ID is not set.");
  }

  std::string url = base_url_ + ":retrieveUserQuota";
  std::vector<std::string> headers = {"Content-Type: application/json", "Authorization: Bearer " + oauth_token};

  nlohmann::json body;
  body["project"] = project_id_;

  auto resp_or = http_client_->Post(url, body.dump(), headers);
  if (!resp_or.ok()) return resp_or.status();

  auto j = nlohmann::json::parse(*resp_or, nullptr, false);
  if (j.is_discarded()) return absl::InternalError("Failed to parse quota response");
  return j;
}

}  // namespace slop
