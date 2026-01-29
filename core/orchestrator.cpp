#include "core/orchestrator.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_set>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/substitute.h"
#include "absl/time/clock.h"

#include "core/constants.h"
#include "core/orchestrator_gemini.h"
#include "core/orchestrator_openai.h"
#include "core/system_prompt_data.h"
#ifdef HAVE_SYSTEM_PROMPT_H
#endif

namespace slop {

Orchestrator::Builder::Builder(Database* db, HttpClient* http_client) : db_(db), http_client_(http_client) {}

Orchestrator::Builder::Builder(const Orchestrator& orchestrator)
    : db_(orchestrator.db_), http_client_(orchestrator.http_client_), config_(orchestrator.config_) {}

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

absl::StatusOr<std::unique_ptr<Orchestrator>> Orchestrator::Builder::Build() {
  if (db_ == nullptr) {
    return absl::InvalidArgumentError("Database cannot be null");
  }
  if (http_client_ == nullptr) {
    return absl::InvalidArgumentError("HttpClient cannot be null");
  }
  auto orchestrator = std::unique_ptr<Orchestrator>(new Orchestrator(db_, http_client_));
  BuildInto(orchestrator.get());
  return orchestrator;
}

void Orchestrator::Builder::BuildInto(Orchestrator* orchestrator) {
  orchestrator->config_ = config_;
  orchestrator->UpdateStrategy();
}

Orchestrator::Orchestrator(Database* db, HttpClient* http_client) : db_(db), http_client_(http_client) {}

void Orchestrator::UpdateStrategy() {
  if (config_.provider == Provider::GEMINI) {
    if (config_.gca_mode) {
      strategy_ = std::make_unique<GeminiGcaOrchestrator>(db_, http_client_, config_.model, config_.base_url,
                                                          config_.project_id);
    } else {
      strategy_ = std::make_unique<GeminiOrchestrator>(db_, http_client_, config_.model, config_.base_url);
    }
  } else {
    auto openai = std::make_unique<OpenAiOrchestrator>(db_, http_client_, config_.model, config_.base_url);
    openai->SetStripReasoning(config_.strip_reasoning);
    strategy_ = std::move(openai);
  }
}

/**
 * @brief Constructs the full prompt payload for the LLM.
 *
 * Orchestrates the prompt assembly by:
 * 1. Fetching session context settings (e.g., window size).
 * 2. Retrieving relevant conversation history from the database.
 * 3. Building system instructions including skills and history guidelines.
 * 4. Injecting relevant memos based on history context.
 * 5. delegating the final payload formatting to the strategy (Gemini/OpenAI).
 *
 * @param session_id The active session ID.
 * @param active_skills List of skills currently active for the turn.
 * @return absl::StatusOr<nlohmann::json> The prepared JSON payload for the LLM API.
 */
absl::StatusOr<nlohmann::json> Orchestrator::AssemblePrompt(const std::string& session_id,
                                                            const std::vector<std::string>& active_skills) {
  auto settings_or = db_->GetContextSettings(session_id);
  if (!settings_or.ok()) return settings_or.status();
  if (settings_or->size == -1) {
    last_selected_groups_.clear();
    return nlohmann::json({{"contents", nlohmann::json::array()}});
  }

  auto history_or = GetRelevantHistory(session_id, settings_or->size);
  if (!history_or.ok()) return history_or.status();

  std::string system_instruction = BuildSystemInstructions(session_id, active_skills);
  InjectRelevantMemos(*history_or, &system_instruction);
  return strategy_->AssemblePayload(session_id, system_instruction, *history_or);
}

absl::StatusOr<int> Orchestrator::ProcessResponse(const std::string& session_id, const std::string& response_json,
                                                  const std::string& group_id) {
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

int Orchestrator::CountTokens(const nlohmann::json& prompt) { return strategy_->CountTokens(prompt); }

/**
 * @brief Constructs the system instruction string for the LLM.
 *
 * Combines the builtin system prompt, conversation history guidelines,
 * and the definitions/usage instructions for any active skills.
 *
 * @param session_id The active session ID.
 * @param active_skills List of skill names to include in the instructions.
 * @return std::string The complete system instruction string.
 */
std::string Orchestrator::BuildSystemInstructions(const std::string& session_id,
                                                  const std::vector<std::string>& active_skills) {
  static constexpr absl::string_view kHistoryInstructions = R"(
## Conversation History Guidelines
1. The following messages are sequential and chronological.
2. Every response MUST include a ### STATE block at the end to summarize technical progress.
3. Use the ### STATE block from the history as the authoritative source for project goals and technical anchors.

### State Format
### STATE
Goal: [Short description of current task]
Context: [Active files/classes being edited]
Resolved: [List of things finished this session]
Technical Anchors: [Ports, IPs, constant values]
)";

  std::string system_instruction;
#ifdef HAVE_SYSTEM_PROMPT_H
  {
    // The builtin system prompt may contain metadata headers like #patch: or #purpose:.
    // This loop extracts only the content that follows these headers, allowing
    // for a clean separation of the actual instruction from development-time notes.
    std::stringstream ss(kBuiltinSystemPrompt);
    std::string line;
    bool in_patch = false;
    while (std::getline(ss, line)) {
      absl::string_view s = absl::StripLeadingAsciiWhitespace(line);
      if (absl::StartsWith(s, "#patch:") || absl::StartsWith(s, "#purpose:") || absl::StartsWith(s, "# patch:") ||
          absl::StartsWith(s, "# purpose:")) {
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
    absl::StrAppend(&system_instruction, "\n## Available Tools\n",
                    "You have access to the following tools. Use them to fulfill the user's request.\n");
    for (const auto& t : *tools_or) {
      absl::StrAppend(&system_instruction, "- ", t.name, ": ", t.description, "\n");
    }
  }

  auto all_skills_or = db_->GetSkills();
  if (all_skills_or.ok() && !active_skills.empty()) {
    absl::StrAppend(&system_instruction, "\n## Active Personas & Skills\n");
    for (const auto& skill : *all_skills_or) {
      for (const auto& active_name : active_skills) {
        if (skill.name == active_name) {
          absl::StrAppend(&system_instruction, "### Skill: ", skill.name, "\n", skill.system_prompt_patch, "\n");
        }
      }
    }
  }

  absl::StrAppend(&system_instruction, kHistoryInstructions, "\n");

  auto state_or = db_->GetSessionState(session_id);
  if (state_or.ok() && !state_or->empty()) {
    absl::StrAppend(&system_instruction, "## Global State (Anchor)\n", *state_or, "\n");
  }

  return system_instruction;
}

absl::StatusOr<std::vector<Database::Message>> Orchestrator::GetRelevantHistory(const std::string& session_id,
                                                                                int window_size) {
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
      auto state = ExtractState(msg.content);
      if (state) {
        db_->SetSessionState(session_id, *state).IgnoreError();
      }
    }
  }
  return absl::OkStatus();
}

void Orchestrator::InjectRelevantMemos(const std::vector<Database::Message>& history, std::string* system_instruction) {
  if (history.empty()) return;

  // Find the last user message
  std::string last_user_text;
  for (auto it = history.rbegin(); it != history.rend(); ++it) {
    if (it->role == "user") {
      last_user_text = it->content;
      break;
    }
  }
  if (last_user_text.empty()) return;

  std::vector<std::string> tags = Database::ExtractTags(last_user_text);
  if (tags.empty()) return;

  auto memos_or = db_->GetMemosByTags(tags);
  if (memos_or.ok() && !memos_or->empty()) {
    absl::StrAppend(system_instruction, "\n## Relevant Memos\n",
                    "The following memos were automatically retrieved as they might be relevant to the "
                    "current context:\n");
    // Limit to top 5 memos to avoid clutter
    int count = 0;
    for (const auto& m : *memos_or) {
      absl::StrAppend(system_instruction, "- [", m.semantic_tags, "] ", m.content, "\n");
      if (++count >= 5) break;
    }
  }
}

std::string Orchestrator::SmarterTruncate(const std::string& content, size_t limit) {
  if (content.size() <= limit) return content;
  std::string truncated = content.substr(0, limit);
  std::string metadata = absl::Substitute(
      "\n... [TRUNCATED: Showing $0/$1 characters. Use the tool again with an offset to read more.] ...", limit,
      content.size());
  return truncated + metadata;
}

std::optional<std::string> Orchestrator::ExtractState(const std::string& text) {
  size_t start_pos = text.find("### STATE");
  if (start_pos == std::string::npos) return std::nullopt;

  // Find the next header or the end of the message to terminate the state block.
  // We look for headers (starts with #) or thematic breaks (---)
  size_t end_pos = text.find("\n#", start_pos + 9);
  if (end_pos == std::string::npos) {
    end_pos = text.find("\n---", start_pos + 9);
  }

  std::string state_blob;
  if (end_pos != std::string::npos) {
    state_blob = text.substr(start_pos, end_pos - start_pos);
  } else {
    state_blob = text.substr(start_pos);
  }
  return std::string(absl::StripAsciiWhitespace(state_blob));
}

}  // namespace slop
