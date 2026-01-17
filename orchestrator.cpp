#include "orchestrator.h"
#include "constants.h"
#include <iostream>
#include <map>
#include <algorithm>
#include <set>
#include <fstream>
#include <sstream>
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/substitute.h"

#ifdef HAVE_SYSTEM_PROMPT_H
#include "system_prompt_data.h"
#endif

namespace slop {

Orchestrator::Orchestrator(Database* db, HttpClient* http_client)
    : db_(db), http_client_(http_client), throttle_(0) {}

absl::StatusOr<nlohmann::json> Orchestrator::AssemblePrompt(const std::string& session_id, const std::vector<std::string>& active_skills) {
  // 1. Check for 'Dropped' context state
  auto settings_or = db_->GetContextSettings(session_id);
  if (!settings_or.ok()) return settings_or.status();
  if (settings_or->size == -1) {
      last_selected_groups_.clear();
      return nlohmann::json({{"contents", nlohmann::json::array()}});
  }

  // 2. Fetch and window conversation history
  auto history_or = GetRelevantHistory(session_id, settings_or->size);
  if (!history_or.ok()) return history_or.status();
  
  // 3. Assemble top-level system instructions (character, tools, state)
  std::string system_instruction = BuildSystemInstructions(session_id, active_skills);

  // 4. Format for specific provider
  if (provider_ == Provider::GEMINI) {
      auto payload = FormatGeminiPayload(system_instruction, *history_or);
      
      // Handle GCA wrapping if enabled
      if (gca_mode_) {
          nlohmann::json wrapped;
          wrapped["model"] = model_;
          wrapped["project"] = project_id_;
          wrapped["user_prompt_id"] = std::to_string(absl::ToUnixNanos(absl::Now()));
          nlohmann::json inner_request = payload;
          inner_request["session_id"] = session_id;
          wrapped["request"] = inner_request;
          return wrapped;
      }
      return payload;
  } else {
      return FormatOpenAIPayload(system_instruction, *history_or);
  }
}

std::string Orchestrator::BuildSystemInstructions(const std::string& session_id, const std::vector<std::string>& active_skills) {
  const char* kHistoryInstructions = R"(
---CONVERSATION HISTORY GUIDELINES---
1. The following messages are sequential and chronological.
2. Every response MUST include a ---STATE--- block at the end to summarize technical progress.
3. Use the ---STATE--- block from the history as the authoritative source for project goals and technical anchors.
---STATE FORMAT---
Goal: [Short description of current task]
Context: [Active files/classes being edited]
Resolved: [List of things finished this session]
Technical Anchors: [Ports, IPs, constant values]
---END STATE---
)";

  std::string system_instruction;
#ifdef HAVE_SYSTEM_PROMPT_H
  {
      std::stringstream ss(kBuiltinSystemPrompt);
      std::string line;
      bool in_patch = false;
      while (std::getline(ss, line)) {
          if (absl::StartsWith(line, "#patch:") || absl::StartsWith(line, "#purpose:")) {
              in_patch = true;
              continue;
          }
          if (in_patch) {
              system_instruction += line + "\n";
          }
      }
  }
#endif

  if (system_instruction.empty()) {
      system_instruction = "You are a helpful coding assistant.";
  }

  if (!system_instruction.empty() && system_instruction.back() != '\n') system_instruction += "\n";

  // Inject Available Tools (for character awareness, even if function declarations exist)
  auto tools_or = db_->GetEnabledTools();
  if (tools_or.ok() && !tools_or->empty()) {
      system_instruction += "\n---AVAILABLE TOOLS---\n";
      system_instruction += "You have access to the following tools. Use them to fulfill the user's request.\n";
      for (const auto& t : *tools_or) {
          system_instruction += "- " + t.name + ": " + t.description + "\n";
      }
  }

  // Inject Active Skills/Personas
  auto all_skills_or = db_->GetSkills();
  if (all_skills_or.ok() && !active_skills.empty()) {
      system_instruction += "\n---ACTIVE PERSONAS & SKILLS---\n";
      for (const auto& skill : *all_skills_or) {
          for (const auto& active_name : active_skills) {
              if (skill.name == active_name) {
                  system_instruction += "### Skill: " + skill.name + "\n";
                  system_instruction += skill.system_prompt_patch + "\n";
              }
          }
      }
  }

  // Inject History Guidelines
  system_instruction += kHistoryInstructions;
  system_instruction += "\n";

  // Inject Global Anchor (Session State)
  auto state_or = db_->GetSessionState(session_id);
  if (state_or.ok() && !state_or->empty()) {
      system_instruction += "---GLOBAL STATE (ANCHOR)---\n";
      system_instruction += *state_or + "\n";
  }

  return system_instruction;
}

absl::StatusOr<std::vector<Database::Message>> Orchestrator::GetRelevantHistory(const std::string& session_id, int window_size) {
  auto hist_or = db_->GetConversationHistory(session_id, false);
  if (!hist_or.ok()) return hist_or.status();
  std::vector<Database::Message> history = std::move(*hist_or);

  if (window_size > 0 && !history.empty()) {
      // Find the last N distinct group_ids
      std::vector<std::string> chron_groups;
      std::set<std::string> seen;
      for (auto it = history.rbegin(); it != history.rend(); ++it) {
          if (seen.find(it->group_id) == seen.end()) {
              chron_groups.push_back(it->group_id);
              seen.insert(it->group_id);
          }
      }
      
      size_t limit = static_cast<size_t>(window_size);
      if (chron_groups.size() > limit) {
          std::set<std::string> keep_groups;
          for (size_t i = 0; i < limit; ++i) keep_groups.insert(chron_groups[i]);
          
          std::vector<Database::Message> filtered;
          for (const auto& m : history) {
              if (keep_groups.count(m.group_id)) filtered.push_back(m);
          }
          history = std::move(filtered);
      }
  }

  // Update selection tracking for UI/Debug purposes
  std::set<std::string> group_ids;
  for (const auto& m : history) group_ids.insert(m.group_id);
  last_selected_groups_ = std::vector<std::string>(group_ids.begin(), group_ids.end());

  return history;
}

std::string Orchestrator::SmarterTruncate(const std::string& content, size_t limit) {
    if (content.size() <= limit) return content;
    
    std::string truncated = content.substr(0, limit);
    std::string metadata = absl::Substitute(
        "\n... [TRUNCATED: Showing $0/$1 characters. Use the tool again with an offset to read more.] ...",
        limit, content.size());
    return truncated + metadata;
}

// Gemini API uses a "contents" array where each entry has a "role" and an array of "parts".
// Strict requirements:
// 1. Roles must be 'user' or 'model'.
// 2. Consecutive turns with the same role are FORBIDDEN (must be merged into parts).
// 3. Tool results use the role 'function' and must follow a 'model' turn.
// 4. System instructions are sent in a separate top-level object.
nlohmann::json Orchestrator::FormatGeminiPayload(const std::string& system_instruction, const std::vector<Database::Message>& history) {
  nlohmann::json payload;
  nlohmann::json contents = nlohmann::json::array();
  
  for (size_t i = 0; i < history.size(); ++i) {
      const auto& msg = history[i];
      std::string display_content = msg.content;

      // Add structural markers to the very first message and the current request
      if (i == 0) display_content = "--- BEGIN CONVERSATION HISTORY ---\n" + display_content;
      if (i == history.size() - 1 && msg.role == "user" && i > 0) {
          display_content = "--- END OF HISTORY ---\n\n### CURRENT REQUEST\n" + display_content;
      }

      // Skip system messages in the history block (they were moved to system_instruction)
      if (msg.role == "system") continue; 

      std::string role = (msg.role == "assistant") ? "model" : (msg.role == "tool" ? "function" : msg.role);
      nlohmann::json part;
      auto j = nlohmann::json::parse(msg.content, nullptr, false);
      
      // Handle Tool Response Truncation
      if (!j.is_discarded() && j.contains("functionResponse")) {
          std::string raw_content = j["functionResponse"]["response"]["content"].get<std::string>();
          j["functionResponse"]["response"]["content"] = SmarterTruncate(raw_content, kMaxToolResultContext);
          part = j;
      }
      else if (!j.is_discarded() && j.contains("functionCall")) part = j;
      else part = {{"text", display_content}};

      // Group consecutive messages with same role (Gemini requirement)
      if (!contents.empty() && contents.back()["role"] == role) contents.back()["parts"].push_back(part);
      else contents.push_back({{"role", role}, {"parts", {part}}});
  }

  // Filter out any trailing function turns that don't have a preceding model turn (Gemini requirement)
  nlohmann::json valid_contents = nlohmann::json::array();
  for (const auto& c : contents) {
      if (c["role"] == "function" && (valid_contents.empty() || valid_contents.back()["role"] != "model")) continue;
      valid_contents.push_back(c);
  }

  payload["contents"] = valid_contents;
  if (!system_instruction.empty()) payload["system_instruction"] = {{"parts", {{{"text", system_instruction}}}}};
  
  // Inject function declarations
  nlohmann::json f_decls = nlohmann::json::array();
  auto tools_or = db_->GetEnabledTools();
  if (tools_or.ok()) {
      for (const auto& t : *tools_or) {
          auto schema = nlohmann::json::parse(t.json_schema, nullptr, false);
          if (!schema.is_discarded()) f_decls.push_back({{"name", t.name}, {"description", t.description}, {"parameters", schema}});
      }
  }
  if (!f_decls.empty()) payload["tools"] = {{{"function_declarations", f_decls}}};
  
  return payload;
}

// OpenAI API uses a flat "messages" array.
// Characteritics:
// 1. Roles are 'system', 'user', 'assistant', and 'tool'.
// 2. Tool results must include a 'tool_call_id'.
// 3. System instruction is simply the first message in the array.
nlohmann::json Orchestrator::FormatOpenAIPayload(const std::string& system_instruction, const std::vector<Database::Message>& history) {
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

      nlohmann::json msg_obj = {{"role", msg.role}};
      auto j = nlohmann::json::parse(msg.content, nullptr, false);
      if (!j.is_discarded()) {
          if (j.contains("tool_calls")) {
              msg_obj["tool_calls"] = j["tool_calls"];
              msg_obj["content"] = nullptr;
          }
          else if (msg.role == "tool" && j.contains("content")) { 
              msg_obj["tool_call_id"] = msg.tool_call_id.substr(0, msg.tool_call_id.find('|')); 
              msg_obj["content"] = SmarterTruncate(j["content"].get<std::string>(), kMaxToolResultContext);
          }
          else msg_obj["content"] = display_content;
      } else msg_obj["content"] = display_content;

      // OpenAI Merge User Turns
      if (!messages.empty() && messages.back()["role"] == msg.role && msg.role == "user") {
          messages.back()["content"] = messages.back()["content"].get<std::string>() + "\n" + msg_obj["content"].get<std::string>();
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
              tools.push_back({{"type", "function"}, {"function", {{"name", t.name}, {"description", t.description}, {"parameters", schema}}}});
          }
      }
  }
  if (!tools.empty()) payload["tools"] = tools;
  
  return payload;
}


absl::StatusOr<std::vector<std::string>> Orchestrator::GetModels(const std::string& api_key) {
    std::string url;
    std::vector<std::string> headers = {"Content-Type: application/json"};
    if (provider_ == Provider::GEMINI) {
        if (gca_mode_) {
            url = base_url_ + "/models";
            if (!api_key.empty()) {
                headers.push_back("Authorization: Bearer " + api_key);
            }
        } else {
            // For public Gemini API, if we have an OAuth token (starts with ya29 or similar), 
            // use Authorization header. If it's a short key, use query param.
            if (!api_key.empty() && api_key.length() > 50) {
                url = absl::StrCat(kPublicGeminiBaseUrl, "/models");
                headers.push_back("Authorization: Bearer " + api_key);
            } else {
                url = absl::StrCat(kPublicGeminiBaseUrl, "/models?key=", api_key);
            }
        }
    } else {
        url = absl::StrCat(kOpenAIBaseUrl, "/models");
        headers.push_back("Authorization: Bearer " + api_key);
    }

    auto resp_or = http_client_->Get(url, headers);
    if (!resp_or.ok()) {
        if (absl::IsInternal(resp_or.status()) && absl::StrContains(resp_or.status().message(), "HTTP error 404")) {
            return absl::NotFoundError("The models endpoint was not found (404). This might not be supported for the current provider/mode.");
        }
        return resp_or.status();
    }

    auto j = nlohmann::json::parse(*resp_or, nullptr, false);
    if (j.is_discarded()) return absl::InternalError("Failed to parse models response");

    std::vector<std::string> models;
    if (provider_ == Provider::GEMINI) {
        if (j.contains("models")) {
            for (const auto& m : j["models"]) {
                std::string name = m["name"];
                if (absl::StartsWith(name, "models/")) name = name.substr(7);
                models.push_back(name);
            }
        }
    } else {
        if (j.contains("data")) {
            for (const auto& m : j["data"]) {
                models.push_back(m["id"]);
            }
        }
    }
    std::sort(models.begin(), models.end());
    return models;
}

absl::StatusOr<nlohmann::json> Orchestrator::GetQuota(const std::string& oauth_token) {
    if (!gca_mode_) {
        return absl::UnimplementedError("Quota reporting is only available in Antigravity/GCA mode.");
    }
    if (project_id_.empty()) {
        return absl::FailedPreconditionError("Project ID is not set.");
    }

    std::string url = base_url_ + ":retrieveUserQuota";
    std::vector<std::string> headers = {
        "Content-Type: application/json",
        "Authorization: Bearer " + oauth_token
    };

    nlohmann::json body;
    body["project"] = project_id_;

    auto resp_or = http_client_->Post(url, body.dump(), headers);
    if (!resp_or.ok()) return resp_or.status();

    auto j = nlohmann::json::parse(*resp_or, nullptr, false);
    if (j.is_discarded()) return absl::InternalError("Failed to parse quota response");

    return j;
}

absl::Status Orchestrator::ProcessResponse(const std::string& session_id, const std::string& response_json, const std::string& group_id) {
  auto j = nlohmann::json::parse(response_json, nullptr, false);
  if (j.is_discarded()) return absl::InternalError("Failed to parse LLM response");

  absl::Status status = absl::InternalError("Unknown response format");
  if (provider_ == Provider::GEMINI) {
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

      if (target->contains("candidates") && !(*target)["candidates"].empty()) {
          auto& parts = (*target)["candidates"][0]["content"]["parts"];
          for (const auto& part : parts) {
              if (part.contains("functionCall")) status = db_->AppendMessage(session_id, "assistant", part.dump(), part["functionCall"]["name"], "tool_call", group_id);
              else if (part.contains("text")) {
                  std::string text = part["text"];
                  status = db_->AppendMessage(session_id, "assistant", text, "", "completed", group_id);
                  
                  // Extract State
                  size_t start_pos = text.find("---STATE---");
                  if (start_pos != std::string::npos) {
                      size_t end_pos = text.find("---END STATE---", start_pos);
                      std::string state_blob;
                      if (end_pos != std::string::npos) {
                          state_blob = text.substr(start_pos, end_pos - start_pos + 15); // Include marker
                      } else {
                          state_blob = text.substr(start_pos);
                      }
                      (void)db_->SetSessionState(session_id, state_blob);
                  }
              }
          }
      }
  } else {
      if (j.contains("usage")) {
        auto& usage = j["usage"];
        int prompt = usage.value("prompt_tokens", 0);
        int completion = usage.value("completion_tokens", 0);
        (void)db_->RecordUsage(session_id, model_, prompt, completion);
      }

      if (j.contains("choices") && !j["choices"].empty()) {
          auto& msg = j["choices"][0]["message"];
          if (msg.contains("tool_calls") && !msg["tool_calls"].empty()) status = db_->AppendMessage(session_id, "assistant", msg.dump(), msg["tool_calls"][0]["id"].get<std::string>() + "|" + msg["tool_calls"][0]["function"]["name"].get<std::string>(), "tool_call", group_id);
          else if (msg.contains("content") && !msg["content"].is_null()) {
              std::string text = msg["content"];
              status = db_->AppendMessage(session_id, "assistant", text, "", "completed", group_id);

              // Extract State
              size_t start_pos = text.find("---STATE---");
              if (start_pos != std::string::npos) {
                  size_t end_pos = text.find("---END STATE---", start_pos);
                  std::string state_blob;
                  if (end_pos != std::string::npos) {
                      state_blob = text.substr(start_pos, end_pos - start_pos + 15); // Include marker
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

absl::StatusOr<Orchestrator::ToolCall> Orchestrator::ParseToolCall(const Database::Message& msg) {
    if (msg.status != "tool_call") return absl::InvalidArgumentError("Not a tool call");
    auto j = nlohmann::json::parse(msg.content, nullptr, false);
    if (j.is_discarded()) return absl::InternalError("JSON error");
    ToolCall tc;
    if (provider_ == Provider::GEMINI) {
        tc.name = msg.tool_call_id;
        tc.args = (j.contains("functionCall") && j["functionCall"].contains("args")) ? j["functionCall"]["args"] : (j.contains("args") ? j["args"] : nlohmann::json::object());
    } else {
        auto& first = j["tool_calls"][0];
        tc.name = first["function"]["name"]; tc.id = first["id"];
        tc.args = nlohmann::json::parse(first["function"]["arguments"].get<std::string>(), nullptr, false);
    }
    return tc;
}


int Orchestrator::CountTokens(const nlohmann::json& prompt) {
    // Basic estimation: ~4 characters per token
    return prompt.dump().length() / 4;
}


absl::Status Orchestrator::RebuildContext(const std::string& session_id) {
  auto settings_or = db_->GetContextSettings(session_id);
  if (!settings_or.ok()) return settings_or.status();
  
  auto history_or = GetRelevantHistory(session_id, settings_or->size);
  if (!history_or.ok()) return history_or.status();
  
  const auto& history = *history_or;
  for (auto it = history.rbegin(); it != history.rend(); ++it) {
      if (it->role == "assistant" || it->role == "model") {
          size_t start_pos = it->content.find("---STATE---");
          if (start_pos != std::string::npos) {
              size_t end_pos = it->content.find("---END STATE---", start_pos);
              std::string state_blob;
              if (end_pos != std::string::npos) {
                  state_blob = it->content.substr(start_pos, end_pos - start_pos + 15);
              } else {
                  state_blob = it->content.substr(start_pos);
              }
              return db_->SetSessionState(session_id, state_blob);
          }
      }
  }
  return absl::NotFoundError("No state block found in current context window.");
}
}  // namespace slop
