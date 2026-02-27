#include "core/orchestrator_gemini.h"

#include <iostream>

#include "absl/container/flat_hash_set.h"
#include "absl/log/log.h"
#include "absl/strings/substitute.h"
#include "absl/time/clock.h"

#include "core/constants.h"
#include "core/message_parser.h"
#include "core/orchestrator.h"
#include "json_utils.h"
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

    std::string role = (msg.role == "assistant") ? "model" : (msg.role == "tool" ? "function" : msg.role);
    nlohmann::json part;

    if (msg.status == "tool_call") {
      auto j_opt = json_parse(msg.content);
      if (j_opt && !j_opt->is_discarded()) {
        auto& j = *j_opt;
        bool valid = true;
        if (const auto* fc = json_at(j, "functionCall")) {
          std::string name = json_get_or(*fc, "name", std::string{});
          if (!enabled_tool_names.contains(name)) {
            LOG(WARNING) << "Filtering out invalid tool call: " << name;
            valid = false;
          }
        }
        if (valid) {
          part = j;
        } else {
          part = {{"text", "[Invalid tool call suppressed: " + msg.content + "]"}};
        }
      } else {
        part = {{"text", display_content}};
      }
    } else if (msg.role == "tool") {
      bool valid = true;
      std::string name = msg.tool_call_id.substr(msg.tool_call_id.find('|') + 1);
      if (!enabled_tool_names.contains(name)) {
        LOG(WARNING) << "Filtering out invalid tool response: " << name;
        valid = false;
      }

      if (valid) {
        part = {{"functionResponse", {{"name", name}, {"response", {{"content", msg.content}}}}}};
      } else {
        role = "user";
        part = {{"text", "[Invalid tool response suppressed]"}};
      }
    } else {
      part = {{"text", display_content}};
    }

    if (!contents.empty() && json_get_or(contents.back(), "role", std::string{}) == role) {
      if (const auto* parts = json_at(contents.back(), "parts")) {
        // Since we know it's our own constructed array, we can append to it.
        // We use the mutable back() reference to avoid const issues.
        contents.back()["parts"].push_back(part);
      }
    } else {
      contents.push_back({{"role", role}, {"parts", {part}}});
    }
  }

  nlohmann::json valid_contents = nlohmann::json::array();
  for (const auto& c : contents) {
    if (json_get_or(c, "role", std::string{}) == "function" &&
        (valid_contents.empty() || json_get_or(valid_contents.back(), "role", std::string{}) != "model"))
      continue;
    valid_contents.push_back(c);
  }

  payload["contents"] = valid_contents;
  if (!system_instruction.empty()) payload["system_instruction"] = {{"parts", {{{"text", system_instruction}}}}};

  nlohmann::json f_decls = nlohmann::json::array();
  if (tools_or.ok()) {
    for (const auto& t : *tools_or) {
      auto it = tool_schema_cache_.find(t.name);
      if (it == tool_schema_cache_.end()) {
        auto schema_opt = json_parse(t.json_schema);
        if (schema_opt) {
          it = tool_schema_cache_.emplace(t.name, std::move(*schema_opt)).first;
        }
      }
      if (it != tool_schema_cache_.end()) {
        f_decls.push_back({{"name", t.name}, {"description", t.description}, {"parameters", it->second}});
      }
    }
  }

  if (!f_decls.empty()) {
    payload["tools"] = {{{"function_declarations", f_decls}}};
    payload["tool_config"] = {{"function_calling_config", {{"mode", "AUTO"}}}};
  }

  payload["generation_config"] = {
      {"temperature", temperature_},
      {"topP", top_p_},
      {"topK", top_k_},
      {"maxOutputTokens", max_output_tokens_},
  };

  return payload;
}
absl::StatusOr<int> GeminiOrchestrator::ProcessResponse(const std::string& session_id, const std::string& response_json,
                                                        const std::string& group_id) {
  auto j_opt = json_parse(response_json);
  if (!j_opt) {
    LOG(ERROR) << "Failed to parse Gemini response: " << response_json;
    return absl::InternalError("Failed to parse LLM response");
  }
  auto& j = *j_opt;

  const nlohmann::json* target = &j;
  if (const auto* response = json_at(j, "response"); response && response->is_object()) {
    target = response;
  }

  int total_tokens = 0;
  if (const auto* usage = json_at(*target, "usageMetadata")) {
    int prompt = json_get_or(*usage, "promptTokenCount", 0);
    int completion = json_get_or(*usage, "candidatesTokenCount", 0);
    total_tokens = prompt + completion;
    (void)db_->RecordUsage(session_id, model_, prompt, completion);
  }

  absl::Status status = absl::InternalError("No candidates in response");
  if (auto candidates = json_get<nlohmann::json::array_t>(*target, "candidates"); candidates && !candidates->empty()) {
    auto& candidate = (*candidates)[0];
    if (const auto* content = json_at(candidate, "content")) {
      if (auto parts = json_get<nlohmann::json::array_t>(*content, "parts")) {
        for (const auto& part : *parts) {
          if (const auto* fc = json_at(part, "functionCall")) {
            std::string name = json_get_or(*fc, "name", std::string{});
            status = db_->AppendMessage(session_id, "assistant", json_dump(part), name, "tool_call", group_id,
                                        GetName(), total_tokens);
          } else if (auto text = json_get<std::string>(part, "text")) {
            status =
                db_->AppendMessage(session_id, "assistant", *text, "", "completed", group_id, GetName(), total_tokens);

            auto state = Orchestrator::ExtractState(*text);
            if (state) {
              db_->SetSessionState(session_id, *state).IgnoreError();
            }
          }
        }
      }
    } else {
      return absl::InternalError("Gemini response candidate missing 'content'");
    }
  }
  if (!status.ok()) return status;
  return total_tokens;
}

absl::StatusOr<std::vector<ToolCall>> GeminiOrchestrator::ParseToolCalls(const Database::Message& msg) {
  return MessageParser::ExtractToolCalls(MessageContext(msg));
}

absl::StatusOr<std::vector<ModelInfo>> GeminiOrchestrator::GetModels(const std::string& api_key) {
  std::string base = base_url_.empty() ? std::string(slop::kPublicGeminiBaseUrl) : base_url_;
  // If the base URL contains a model name or generateContent suffix, strip it
  // to get the base endpoint for model listing.
  if (size_t pos = base.find("/models/"); pos != std::string::npos) {
    base = base.substr(0, pos);
  } else if (size_t pos = base.find("/models"); pos != std::string::npos) {
    // Also handle cases where it might end with /models without a trailing slash
    base = base.substr(0, pos);
  }

  std::string url = base + "/models?key=" + api_key;
  auto resp_or = http_client_->Get(url, {std::string("User-Agent: ") + kUserAgent});
  if (!resp_or.ok()) return resp_or.status();

  auto j_opt = json_parse(*resp_or);
  if (!j_opt) return absl::InternalError("Failed to parse models response");
  auto& j = *j_opt;

  std::vector<ModelInfo> models;
  if (auto models_json = json_get<nlohmann::json::array_t>(j, "models")) {
    for (const auto& m : *models_json) {
      ModelInfo info;
      info.id = json_get_or(m, "name", std::string{});
      info.name = json_get_or(m, "displayName", std::string{});
      models.push_back(info);
    }
  }
  return models;
}

absl::StatusOr<nlohmann::json> GeminiOrchestrator::GetQuota(const std::string& oauth_token) {
  (void)oauth_token;
  return absl::UnimplementedError("Quota check not implemented for Gemini Strategy yet");
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

absl::StatusOr<int> GeminiGcaOrchestrator::ProcessResponse(const std::string& session_id,
                                                           const std::string& response_json,
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
  std::vector<std::string> headers = {"Content-Type: application/json",
                                      "Authorization: Bearer " + oauth_token};
  headers.push_back(std::string("User-Agent: ") + kUserAgent);
  headers.push_back(std::string("x-goog-api-client: ") + kGcaApiClient);
  headers.push_back(std::string("x-goog-api-client-metadata: ") + kGcaClientMetadata);


  nlohmann::json body;
  body["project"] = project_id_;

  auto resp_or = http_client_->Post(url, body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), headers);
  if (!resp_or.ok()) return resp_or.status();

  auto j_opt = json_parse(*resp_or);
  if (!j_opt) return absl::InternalError("Failed to parse quota response");
  return *j_opt;
}

}  // namespace slop
