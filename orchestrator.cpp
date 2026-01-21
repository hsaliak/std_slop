#include "orchestrator.h"
#include "orchestrator_gemini.h"
#include "orchestrator_openai.h"
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
#include "absl/time/clock.h"

#ifdef HAVE_SYSTEM_PROMPT_H
#include "system_prompt_data.h"
#endif

namespace slop {

Orchestrator::Orchestrator(Database* db, HttpClient* http_client)
    : db_(db), http_client_(http_client), throttle_(0) {
}

void Orchestrator::SetProvider(Provider provider) {
    provider_ = provider;
}

void Orchestrator::SetBaseUrl(const std::string& base_url) {
    base_url_ = base_url;
}

void Orchestrator::SetModel(const std::string& model) {
    model_ = model;
}

void Orchestrator::SetGcaMode(bool enabled) {
    gca_mode_ = enabled;
}

void Orchestrator::SetProjectId(const std::string& project_id) {
    project_id_ = project_id;
}

void Orchestrator::UpdateStrategy() {
    if (provider_ == Provider::GEMINI) {
        if (gca_mode_) {
            strategy_ = std::make_unique<GeminiGcaOrchestrator>(db_, http_client_, model_, base_url_, project_id_);
        } else {
            strategy_ = std::make_unique<GeminiOrchestrator>(db_, http_client_, model_, base_url_);
        }
    } else {
        strategy_ = std::make_unique<OpenAiOrchestrator>(db_, http_client_, model_, base_url_);
    }
    strategy_->SetOrchestrator(this);
}

absl::StatusOr<nlohmann::json> Orchestrator::AssemblePrompt(const std::string& session_id, const std::vector<std::string>& active_skills) {
  auto settings_or = db_->GetContextSettings(session_id);
  if (!settings_or.ok()) return settings_or.status();
  if (settings_or->size == -1) {
      last_selected_groups_.clear();
      return nlohmann::json({{"contents", nlohmann::json::array()}});
  }

  auto history_or = GetRelevantHistory(session_id, settings_or->size);
  if (!history_or.ok()) return history_or.status();
  
  std::string system_instruction = BuildSystemInstructions(session_id, active_skills);
  return strategy_->AssemblePayload(session_id, system_instruction, *history_or);
}

absl::Status Orchestrator::ProcessResponse(const std::string& session_id, const std::string& response_json, const std::string& group_id) {
    return strategy_->ProcessResponse(session_id, response_json, group_id);
}

absl::StatusOr<std::vector<ToolCall>> Orchestrator::ParseToolCalls(const Database::Message& msg) {
    if (msg.parsing_strategy == "openai") {
        OpenAiOrchestrator parser(db_, http_client_, "", "");
        return parser.ParseToolCalls(msg);
    } else if (msg.parsing_strategy == "gemini" || msg.parsing_strategy == "gemini_gca") {
        GeminiOrchestrator parser(db_, http_client_, "", "");
        return parser.ParseToolCalls(msg);
    }
    return strategy_->ParseToolCalls(msg);
}

absl::StatusOr<std::vector<ModelInfo>> Orchestrator::GetModels(const std::string& api_key) {
    return strategy_->GetModels(api_key);
}

absl::StatusOr<nlohmann::json> Orchestrator::GetQuota(const std::string& oauth_token) {
    return strategy_->GetQuota(oauth_token);
}

int Orchestrator::CountTokens(const nlohmann::json& prompt) {
    return strategy_->CountTokens(prompt);
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
          absl::string_view s = absl::StripLeadingAsciiWhitespace(line);
          if (absl::StartsWith(s, "#patch:") || absl::StartsWith(s, "#purpose:") ||
              absl::StartsWith(s, "# patch:") || absl::StartsWith(s, "# purpose:")) {
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

  auto tools_or = db_->GetEnabledTools();
  if (tools_or.ok() && !tools_or->empty()) {
      system_instruction += "\n---AVAILABLE TOOLS---\n";
      system_instruction += "You have access to the following tools. Use them to fulfill the user's request.\n";
      for (const auto& t : *tools_or) {
          system_instruction += "- " + t.name + ": " + t.description + "\n";
      }
  }

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

  system_instruction += kHistoryInstructions;
  system_instruction += "\n";

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
      std::vector<std::string> chron_groups;
      chron_groups.reserve(history.size());
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
          filtered.reserve(history.size());
          for (const auto& m : history) {
              if (keep_groups.count(m.group_id)) filtered.push_back(m);
          }
          history = std::move(filtered);
      }
  }

  std::set<std::string> group_ids;
  for (const auto& m : history) group_ids.insert(m.group_id);
  last_selected_groups_ = std::vector<std::string>(group_ids.begin(), group_ids.end());

  return history;
}

absl::Status Orchestrator::RebuildContext(const std::string& session_id) {
  auto settings_or = db_->GetContextSettings(session_id);
  if (!settings_or.ok()) return settings_or.status();
  auto history_or = GetRelevantHistory(session_id, settings_or->size);
  if (!history_or.ok()) return history_or.status();

  for (const auto& msg : *history_or) {
      if (msg.role == "assistant") {
          size_t start_pos = msg.content.find("---STATE---");
          if (start_pos != std::string::npos) {
              size_t end_pos = msg.content.find("---END STATE---", start_pos);
              std::string state_blob;
              if (end_pos != std::string::npos) {
                  state_blob = msg.content.substr(start_pos, end_pos - start_pos + 15);
              } else {
                  state_blob = msg.content.substr(start_pos);
              }
              (void)db_->SetSessionState(session_id, state_blob);
          }
      }
  }
  return absl::OkStatus();
}

}  // namespace slop
