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
#include "core/json_utils.h"
#include "core/status_macros.h"
#include "core/system_prompt_data.h"
#ifdef HAVE_SYSTEM_PROMPT_H
#endif
namespace slop {
Orchestrator::Builder::Builder(Database* db, HttpClient* http_client) : db_(db), http_client_(http_client) {}
Orchestrator::Builder::Builder(const Orchestrator& orchestrator)
    : db_(orchestrator.db_), http_client_(orchestrator.http_client_), config_(orchestrator.config_) {}
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
  auto orchestrator = std::make_unique<Orchestrator>(db_, http_client_);
  orchestrator->config_ = config_;
  orchestrator->responses_ =
      std::make_unique<OpenAiResponsesOrchestrator>(db_, http_client_, config_.model, config_.base_url);
  (void)orchestrator->LoadAgentMd("./AGENTS.md");
  (void)orchestrator->ReloadAllSkills();
  return orchestrator;
}
Orchestrator::Orchestrator(Database* db, HttpClient* http_client) : db_(db), http_client_(http_client) {}
void Orchestrator::Builder::Apply(Orchestrator* orchestrator) {
  orchestrator->config_ = config_;
  orchestrator->responses_ =
      std::make_unique<OpenAiResponsesOrchestrator>(db_, http_client_, config_.model, config_.base_url);
}
/**
 * @brief Constructs the full prompt payload for the LLM.
 *
 * Orchestrates the prompt assembly by:
 * 1. Fetching session context settings (e.g., window size).
 * 2. Retrieving relevant conversation history from the database.
 * 3. Building system instructions including history guidelines.
 * 4. Formatting the final Responses request.
 *
 * @param session_id The active session ID.
 * @param active_skills List of skills currently active for the turn.
 * @return absl::StatusOr<nlohmann::json> The prepared JSON payload for the LLM API.
 */
absl::StatusOr<nlohmann::json> Orchestrator::AssemblePrompt(const std::string& session_id,
                                                            const std::vector<std::string>& active_skills) {
  auto history_or = GetAccordionHistory(session_id);
  if (!history_or.ok()) return history_or.status();
  auto history = std::move(*history_or);
  for (auto& message : history) {
    if (message.role == "tool") {
      message.content = SmarterTruncate(message.content, config_.truncation.full_fidelity_limit, message.id);
      if (!message.api_item_json.empty()) {
        auto item = json_parse(message.api_item_json);
        if (item && json_get_or(*item, "type", std::string{}) == "function_call_output") {
          (*item)["output"] = message.content;
          message.api_item_json = json_dump(*item);
        }
      }
    }
  }
  std::string system_instruction = BuildSystemInstructions(session_id);
  InjectAgentMd(&system_instruction);
  InjectSkillsSummary(&system_instruction);
  ASSIGN_OR_RETURN(auto enabled_tools, db_->GetTopLevelTools());
  std::string active_skill_content;
  if (!active_skills.empty()) {
    ASSIGN_OR_RETURN(auto skills, db_->GetSkills());
    std::map<std::string, const Database::Skill*> skills_by_name;
    for (const auto& skill : skills) {
      skills_by_name.emplace(skill.name, &skill);
    }
    active_skill_content = "## Active Personas & Skills\n";
    std::vector<std::string> sorted_active_skills = active_skills;
    std::sort(sorted_active_skills.begin(), sorted_active_skills.end());
    for (const auto& active_name : sorted_active_skills) {
      const auto skill_it = skills_by_name.find(active_name);
      if (skill_it != skills_by_name.end()) {
        absl::StrAppend(&active_skill_content, "### Skill: ", skill_it->second->name, "\n",
                        skill_it->second->system_prompt_patch, "\n");
      }
    }
  }
  ResponsesRequestInput request{system_instruction, std::move(history), std::move(enabled_tools),
                                std::move(active_skill_content), structured_output_schema_, session_id};
  auto payload_or = responses_->BuildRequest(request);
  if (payload_or.ok() && std::getenv("SLOP_TOOL_DEBUG")) {
    LOG(INFO) << "--- ASSEMBLED PROMPT ---\n" << payload_or->dump(2) << "\n--- END PROMPT ---";
  }
  return payload_or;
}
absl::StatusOr<nlohmann::json> Orchestrator::AssemblePayload(const std::string& session_id,
                                                            const std::string& system_instruction,
                                                            const std::vector<Database::Message>& history,
                                                            const std::vector<std::string>& active_skills) {
  ASSIGN_OR_RETURN(auto enabled_tools, db_->GetTopLevelTools());
  std::string active_skill_content;
  if (!active_skills.empty()) {
    ASSIGN_OR_RETURN(auto skills, db_->GetSkills());
    std::map<std::string, const Database::Skill*> skills_by_name;
    for (const auto& skill : skills) {
      skills_by_name.emplace(skill.name, &skill);
    }
    active_skill_content = "## Active Personas & Skills\n";
    std::vector<std::string> sorted_active_skills = active_skills;
    std::sort(sorted_active_skills.begin(), sorted_active_skills.end());
    for (const auto& active_name : sorted_active_skills) {
      const auto skill_it = skills_by_name.find(active_name);
      if (skill_it != skills_by_name.end()) {
        absl::StrAppend(&active_skill_content, "### Skill: ", skill_it->second->name, "\n",
                        skill_it->second->system_prompt_patch, "\n");
      }
    }
  }
  return responses_->BuildRequest({system_instruction, history, std::move(enabled_tools),
                                   std::move(active_skill_content), structured_output_schema_, session_id});
}
absl::StatusOr<int> Orchestrator::ProcessResponse(const std::string& session_id, const std::string& response_json,
                                                  const std::string& group_id) {
  return responses_->ProcessResponse(session_id, response_json, group_id);
}
std::optional<ResponseUsage> Orchestrator::GetLastResponseUsage() const {
  return responses_->GetLastResponseUsage();
}

const std::vector<ResponsesOutputItem>& Orchestrator::GetLastOutputItems() const {
  return responses_->GetLastOutputItems();
}

absl::StatusOr<std::vector<ToolCall>> Orchestrator::ParseLastOutputToolCalls() const {
  return responses_->ParseLastOutputToolCalls();
}
absl::StatusOr<std::string> Orchestrator::ExtractAssistantText(const std::string& response_body) {
  return responses_->ExtractAssistantText(response_body);
}
absl::StatusOr<std::vector<ToolCall>> Orchestrator::ParseToolCalls(const Database::Message& msg) {
  return responses_->ParseToolCalls(msg);
}
absl::StatusOr<std::vector<ModelInfo>> Orchestrator::GetModels(const std::string& api_key,
                                                               const std::string& account_id) {
  return responses_->GetModels(api_key, account_id);
}
absl::StatusOr<nlohmann::json> Orchestrator::GetQuota(const std::string& oauth_token) {
  return responses_->GetQuota(oauth_token);
}
/**
 * @brief Constructs the system instruction string for the LLM.
 *
 * Combines the builtin system prompt, tool catalog, AGENTS.md context,
 * and conversation history guidelines. Active skill patches are appended to
 * this fixed instruction prefix when they are enabled.
 *
 * @param session_id The active session ID.
 * @return std::string The complete system instruction string.
 */
std::string Orchestrator::BuildSystemInstructions(const std::string& /*session_id*/) {
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
  auto tools_or = db_->GetTopLevelTools();
  if (tools_or.ok() && !tools_or->empty()) {
    absl::StrAppend(&system_instruction, "\n## Available Tools\n",
                    "You have access to the following tools. Use them to fulfill the user's request.\n");
    for (const auto& t : *tools_or) {
      absl::StrAppend(&system_instruction, "- ", t.name, ": ", t.description, "\n");
    }
  }
  absl::StrAppend(&system_instruction, kHistoryInstructions, "\n");
  return system_instruction;
}
absl::StatusOr<std::vector<Database::Message>> Orchestrator::GetAccordionHistory(
    const std::string& session_id, bool force_reset) {
  ASSIGN_OR_RETURN(auto settings, db_->GetAccordionContextSettings(session_id));
  ASSIGN_OR_RETURN(auto latest_prompt_tokens, db_->GetLatestPromptTokens(session_id));
  const bool reset = force_reset ||
                     (latest_prompt_tokens.has_value() && *latest_prompt_tokens >= settings.watermark_tokens);
  std::vector<std::string> group_ids;
  if (reset) {
    ASSIGN_OR_RETURN(group_ids, db_->GetLastSessionGroupIds(session_id, settings.retain_groups));
    if (!group_ids.empty()) RETURN_IF_ERROR(db_->SetAccordionEpochStartGroup(session_id, group_ids.front()));
  } else {
    ASSIGN_OR_RETURN(group_ids, db_->GetSessionGroupIdsFrom(session_id, settings.epoch_start_group_id));
    if (settings.epoch_start_group_id.empty() && !group_ids.empty()) {
      RETURN_IF_ERROR(db_->SetAccordionEpochStartGroup(session_id, group_ids.front()));
    }
  }
  ASSIGN_OR_RETURN(auto all_messages, db_->GetConversationHistory(session_id));
  const std::set<std::string> selected_groups(group_ids.begin(), group_ids.end());
  std::vector<Database::Message> history;
  history.reserve(all_messages.size());
  for (auto& message : all_messages) {
    const bool in_selected_epoch = selected_groups.empty() ||
                                   selected_groups.find(message.group_id) != selected_groups.end();
    if (in_selected_epoch) history.push_back(std::move(message));
  }
  last_selected_groups_ = std::move(group_ids);
  return history;
}
absl::Status Orchestrator::ForceAccordionReset(const std::string& session_id) {
  auto history_or = GetAccordionHistory(session_id, true);
  return history_or.ok() ? absl::OkStatus() : history_or.status();
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

void Orchestrator::SetStructuredOutputSchema(std::optional<nlohmann::json> schema) {
  structured_output_schema_ = std::move(schema);
}

absl::Status Orchestrator::ReloadAllSkills() {
  (void)ReloadSkills("./.agents/skills");
  (void)ReloadSkills("~/.config/slop/skills");
  return absl::OkStatus();
}
}  // namespace slop
