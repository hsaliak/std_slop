#include "orchestrator.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_set>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/substitute.h"
#include "absl/time/clock.h"

#include "constants.h"
#include "orchestrator_gemini.h"
#include "orchestrator_openai.h"
#include "system_prompt_data.h"
#ifdef HAVE_SYSTEM_PROMPT_H
#endif

namespace slop {

Orchestrator::Builder::Builder(Database* db, HttpClient* http_client)
    : db_(db), http_client_(http_client) {}

Orchestrator::Builder::Builder(const Orchestrator& orchestrator)
    : db_(orchestrator.db_), http_client_(orchestrator.http_client_) {
    config_.provider = orchestrator.provider_;
    config_.model = orchestrator.model_;
    config_.gca_mode = orchestrator.gca_mode_;
    config_.project_id = orchestrator.project_id_;
    config_.base_url = orchestrator.base_url_;
    config_.throttle = orchestrator.throttle_;
    config_.strip_reasoning = orchestrator.strip_reasoning_;
}

Orchestrator::Builder& Orchestrator::Builder::WithProvider(Provider provider) {
    config_.provider = provider;
    return *this;
}

Orchestrator::Builder& Orchestrator::Builder::WithModel(const std::string& model) {
    config_.model = model;
    return *this;
}

Orchestrator::Builder& Orchestrator::Builder::WithGcaMode(bool enabled) {
    config_.gca_mode = enabled;
    return *this;
}

Orchestrator::Builder& Orchestrator::Builder::WithProjectId(const std::string& project_id) {
    config_.project_id = project_id;
    return *this;
}

Orchestrator::Builder& Orchestrator::Builder::WithBaseUrl(const std::string& url) {
    config_.base_url = url;
    return *this;
}

Orchestrator::Builder& Orchestrator::Builder::WithThrottle(int seconds) {
    config_.throttle = seconds;
    return *this;
}

Orchestrator::Builder& Orchestrator::Builder::WithStripReasoning(bool enabled) {
    config_.strip_reasoning = enabled;
    return *this;
}

std::unique_ptr<Orchestrator> Orchestrator::Builder::Build() {
    auto orchestrator = std::unique_ptr<Orchestrator>(new Orchestrator(db_, http_client_));
    BuildInto(orchestrator.get());
    return orchestrator;
}

void Orchestrator::Builder::BuildInto(Orchestrator* orchestrator) {
    orchestrator->provider_ = config_.provider;
    orchestrator->model_ = config_.model;
    orchestrator->gca_mode_ = config_.gca_mode;
    orchestrator->project_id_ = config_.project_id;
    orchestrator->base_url_ = config_.base_url;
    orchestrator->throttle_ = config_.throttle;
    orchestrator->strip_reasoning_ = config_.strip_reasoning;
    orchestrator->UpdateStrategy();
}

Orchestrator::Orchestrator(Database* db, HttpClient* http_client)
    : db_(db), http_client_(http_client), throttle_(0) {
}

void Orchestrator::UpdateStrategy() {
    if (provider_ == Provider::GEMINI) {
        if (gca_mode_) {
            strategy_ = std::make_unique<GeminiGcaOrchestrator>(db_, http_client_, model_, base_url_, project_id_);
        } else {
            strategy_ = std::make_unique<GeminiOrchestrator>(db_, http_client_, model_, base_url_);
        }
    } else {
        auto openai = std::make_unique<OpenAiOrchestrator>(db_, http_client_, model_, base_url_);
        openai->SetStripReasoning(strip_reasoning_);
        strategy_ = std::move(openai);
    }
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
  static constexpr absl::string_view kHistoryInstructions = R"(
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
              absl::StrAppend(&system_instruction, line, "\n");
          }
      }
  }
#endif

  if (system_instruction.empty()) {
      system_instruction = "You are a helpful coding assistant.";
  }

  if (system_instruction.back() != '\n') absl::StrAppend(&system_instruction, "\n");

  auto tools_or = db_->GetEnabledTools();
  if (tools_or.ok() && !tools_or->empty()) {
      absl::StrAppend(&system_instruction, "\n---AVAILABLE TOOLS---\n",
                      "You have access to the following tools. Use them to fulfill the user's request.\n");
      for (const auto& t : *tools_or) {
          absl::StrAppend(&system_instruction, "- ", t.name, ": ", t.description, "\n");
      }
  }

  auto all_skills_or = db_->GetSkills();
  if (all_skills_or.ok() && !active_skills.empty()) {
      absl::StrAppend(&system_instruction, "\n---ACTIVE PERSONAS & SKILLS---\n");
      for (const auto& skill : *all_skills_or) {
          for (const auto& active_name : active_skills) {
              if (skill.name == active_name) {
                  absl::StrAppend(&system_instruction, "### Skill: ", skill.name, "\n",
                                  skill.system_prompt_patch, "\n");
              }
          }
      }
  }

  absl::StrAppend(&system_instruction, kHistoryInstructions, "\n");

  auto state_or = db_->GetSessionState(session_id);
  if (state_or.ok() && !state_or->empty()) {
      absl::StrAppend(&system_instruction, "---GLOBAL STATE (ANCHOR)---\n", *state_or, "\n");
  }

  return system_instruction;
}

absl::StatusOr<std::vector<Database::Message>> Orchestrator::GetRelevantHistory(const std::string& session_id, int window_size) {
  // Use Phase 2 windowed fetching if window_size > 0
  auto hist_or = db_->GetConversationHistory(session_id, false, window_size);
  if (!hist_or.ok()) return hist_or.status();

  std::vector<Database::Message> history;
  history.reserve(hist_or->size());

  const std::string& current_strategy = strategy_->GetName();
  std::set<std::string> group_ids;

  for (auto& m : *hist_or) {
      bool is_tool_related = (m.role == "tool" || m.status == "tool_call");
      bool strategy_matches = (m.parsing_strategy.empty() || m.parsing_strategy == current_strategy ||
                               (current_strategy == "gemini_gca" && m.parsing_strategy == "gemini") ||
                               (current_strategy == "gemini" && m.parsing_strategy == "gemini_gca"));

      if (!is_tool_related || strategy_matches) {
          if (!m.group_id.empty()) {
              group_ids.insert(m.group_id);
          }
          history.push_back(std::move(m));
      }
  }

  last_selected_groups_.assign(group_ids.begin(), group_ids.end());
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

} // namespace slop
