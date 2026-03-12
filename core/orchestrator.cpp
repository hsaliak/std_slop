#include "core/orchestrator.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_set>

#include "absl/log/log.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/substitute.h"
#include "absl/time/clock.h"

#include "core/constants.h"
#include "core/orchestrator_gemini.h"
#include "core/orchestrator_openai.h"
#include "core/orchestrator_openai_responses.h"
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
Orchestrator::Builder& Orchestrator::Builder::WithBaseUrl(const std::string& url) {
  config_.base_url = url;
  return *this;
}
Orchestrator::Builder& Orchestrator::Builder::WithThrottle(int seconds) {
  config_.throttle = seconds;
  return *this;
}
Orchestrator::Builder& Orchestrator::Builder::WithOpenAiApiStyle(OpenAiApiStyle style) {
  config_.openai_api_style = style;
  return *this;
}
Orchestrator::Builder& Orchestrator::Builder::WithDatabase(Database* db) {
  db_ = db;
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
  (void)orchestrator->LoadAgentMd("./AGENTS.md");
  (void)orchestrator->ReloadAllSkills();
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
    strategy_ = std::make_unique<GeminiOrchestrator>(db_, http_client_, config_.model, config_.base_url);
  } else {
    if (config_.openai_api_style == OpenAiApiStyle::RESPONSES) {
      strategy_ = std::make_unique<OpenAiResponsesOrchestrator>(db_, http_client_, config_.model, config_.base_url);
    } else {
      strategy_ = std::make_unique<OpenAiOrchestrator>(db_, http_client_, config_.model, config_.base_url);
    }
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
  auto history = std::move(*history_or);
  // Identify the active group_id (the most recent one)
  std::string active_group_id;
  if (!history.empty()) {
    active_group_id = history.back().group_id;
  }
  // Pre-truncate tool results based on group activity and recency.
  size_t total_active_tools = 0;
  for (const auto& m : history) {
    if (m.role == "tool" && !active_group_id.empty() && m.group_id == active_group_id) {
      total_active_tools++;
    }
  }
  size_t active_tool_idx = 0;
  for (auto& m : history) {
    if (m.role == "tool") {
      bool is_active_group = (!active_group_id.empty() && m.group_id == active_group_id);
      if (!is_active_group) {
        m.content = SmarterTruncate(m.content, config_.truncation.inactive_limit, m.id);
      } else {
        bool is_recent = (active_tool_idx >= (total_active_tools > config_.truncation.full_fidelity_count
                                                  ? total_active_tools - config_.truncation.full_fidelity_count
                                                  : 0));
        size_t limit =
            is_recent ? config_.truncation.active_full_fidelity_limit : config_.truncation.active_degraded_limit;
        m.content = SmarterTruncate(m.content, limit, m.id);
        active_tool_idx++;
      }
    }
  }
  std::string system_instruction = BuildSystemInstructions(session_id, active_skills);
  InjectAgentMd(&system_instruction);
  InjectSkillsSummary(&system_instruction);
  auto payload_or = strategy_->AssemblePayload(session_id, system_instruction, history);
  if (payload_or.ok() && std::getenv("SLOP_TOOL_DEBUG")) {
    LOG(INFO) << "--- ASSEMBLED PROMPT ---\n" << payload_or->dump(2) << "\n--- END PROMPT ---";
  }
  return payload_or;
}
absl::StatusOr<int> Orchestrator::ProcessResponse(const std::string& session_id, const std::string& response_json,
                                                  const std::string& group_id) {
  return strategy_->ProcessResponse(session_id, response_json, group_id);
}
absl::StatusOr<std::vector<ToolCall>> Orchestrator::ParseToolCalls(const Database::Message& msg) {
  return strategy_->ParseToolCalls(msg);
}
absl::StatusOr<std::vector<ModelInfo>> Orchestrator::GetModels(const std::string& api_key,
                                                               const std::string& account_id) {
  return strategy_->GetModels(api_key, account_id);
}
absl::StatusOr<nlohmann::json> Orchestrator::GetQuota(const std::string& oauth_token) {
  return strategy_->GetQuota(oauth_token);
}
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
std::string Orchestrator::SmarterTruncate(const std::string& content, size_t limit, int message_id) {
  if (content.size() <= limit) return content;
  // Sandwich Truncation: 20% Head, 80% Tail.
  // We reserve some space for the truncation hint.
  std::string hint;
  if (message_id > 0) {
    hint = absl::Substitute(
        "\n\n... [TRUNCATED. Use query_db(sql=\"SELECT content FROM messages WHERE id=$0\") to see full output] "
        "...\n\n",
        message_id);
  } else {
    hint = absl::Substitute(
        "\n\n... [TRUNCATED: Showing partial output of $0 bytes. Use query_db or specific tool range to see more.] "
        "...\n\n",
        content.size());
  }
  if (limit <= hint.size() + 10) {
    // If the limit is extremely small, just do basic head truncation to fit.
    size_t tiny_limit = limit > 3 ? limit - 3 : limit;
    while (tiny_limit > 0 && (static_cast<unsigned char>(content[tiny_limit]) & 0xC0) == 0x80) {
      tiny_limit--;
    }
    return content.substr(0, tiny_limit) + "...";
  }
  size_t available_content = limit - hint.size();
  // Ensure we have at least a few characters for head/tail if available_content allows.
  size_t head_size = std::max<size_t>(1, available_content * 0.2);
  size_t tail_size = available_content - head_size;
  // UTF-8 safety for Head (avoid cutting in middle of multi-byte char)
  while (head_size > 0 && (static_cast<unsigned char>(content[head_size]) & 0xC0) == 0x80) {
    head_size--;
  }
  // UTF-8 safety for Tail (ensure we start at a character boundary)
  size_t tail_start = content.size() - tail_size;
  while (tail_start < content.size() && (static_cast<unsigned char>(content[tail_start]) & 0xC0) == 0x80) {
    tail_start++;
  }
  return content.substr(0, head_size) + hint + content.substr(tail_start);
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

absl::Status Orchestrator::LoadAgentMd(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    if (path == "./AGENTS.md") return absl::OkStatus();
    return absl::NotFoundError("Could not open " + path);
  }
  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  active_agent_md_path_ = path;
  return db_->SetAgentMd(path, content);
}

void Orchestrator::InjectAgentMd(std::string* system_instruction) {
  auto content_or = db_->GetAgentMd(active_agent_md_path_);
  if (!content_or.ok() || content_or->empty()) return;
  absl::StrAppend(system_instruction, "\n\n## Project Context (from ", active_agent_md_path_, ")\n", *content_or, "\n");
}

namespace {
std::string ExpandPath(const std::string& path) {
  if (path.empty()) return path;
  if (path[0] == '~') {
    const char* home = std::getenv("HOME");
    if (home) {
      return std::string(home) + path.substr(1);
    }
  }
  return path;
}
}  // namespace

absl::Status Orchestrator::ReloadSkills(const std::string& directory) {
  std::string expanded = ExpandPath(directory);
  if (!std::filesystem::exists(expanded)) {
    // It's okay if it doesn't exist, just return OK (no skills loaded)
    return absl::OkStatus();
  }

  for (const auto& entry : std::filesystem::directory_iterator(expanded)) {
    if (!entry.is_directory()) continue;

    std::filesystem::path skill_file = entry.path() / "SKILL.md";
    if (!std::filesystem::exists(skill_file)) continue;

    std::ifstream file(skill_file);
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    size_t first_dash = content.find("---");
    if (first_dash == std::string::npos) continue;

    size_t second_dash = content.find("---", first_dash + 3);
    if (second_dash == std::string::npos) continue;

    std::string frontmatter = content.substr(first_dash + 3, second_dash - (first_dash + 3));
    std::string body = content.substr(second_dash + 3);

    std::string name_val, desc_val;
    std::istringstream stream(frontmatter);
    std::string line;
    while (std::getline(stream, line)) {
      size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string key = std::string(absl::StripAsciiWhitespace(line.substr(0, colon)));
      std::string val = std::string(absl::StripAsciiWhitespace(line.substr(colon + 1)));
      if (key == "name") name_val = val;
      if (key == "description") desc_val = val;
    }

    if (name_val.empty()) name_val = entry.path().filename().string();

    Database::Skill skill;
    skill.name = name_val;
    skill.description = desc_val;
    skill.system_prompt_patch = std::string(absl::StripAsciiWhitespace(body));

    auto exists_or = db_->SkillExists(skill.name);
    if (exists_or.ok() && *exists_or) {
      (void)(void)db_->UpdateSkill(skill);
    } else {
      (void)(void)db_->RegisterSkill(skill);
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> Orchestrator::ListSkills() const {
  auto skills_or = db_->GetSkills();
  if (!skills_or.ok()) return skills_or.status();
  std::string out;
  for (const auto& skill : *skills_or) {
    absl::StrAppend(&out, "- ", skill.name, ": ", skill.description, "\n");
  }
  return out;
}

void Orchestrator::InjectSkillsSummary(std::string* system_instruction) {
  auto skills_or = db_->GetSkills();
  if (!skills_or.ok() || skills_or->empty()) return;
  absl::StrAppend(system_instruction, "\n\n## Available Skills\n",
                  "Invoke these skills with tools.use_skill in the LCP when relevant to activate\n");
  for (const auto& skill : *skills_or) {
    absl::StrAppend(system_instruction, "- ", skill.name, ": ", skill.description, "\n");
  }
}

absl::Status Orchestrator::ReloadAllSkills() {
  (void)ReloadSkills("./.agents/skills");
  (void)ReloadSkills("~/.config/slop/skills");
  return absl::OkStatus();
}
}  // namespace slop
