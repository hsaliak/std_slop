#include "orchestrator.h"
#include <iostream>
#include <map>
#include <algorithm>
#include <set>
#include "absl/strings/match.h"

namespace slop {

Orchestrator::Orchestrator(Database* db, HttpClient* http_client)
    : db_(db), http_client_(http_client) {}

absl::StatusOr<nlohmann::json> Orchestrator::AssemblePrompt(const std::string& session_id, const std::vector<std::string>& active_skills) {
  auto settings_or = db_->GetContextSettings(session_id);
  if (!settings_or.ok()) return settings_or.status();
  
  std::vector<Database::Message> history;
  if (settings_or->mode == Database::ContextMode::FTS_RANKED) {
      // For RRF, we need to consider ALL groups in this session, even dropped ones.
      auto all_history_or = db_->GetConversationHistory(session_id, true);
      if (!all_history_or.ok()) return all_history_or.status();

      // Hybrid Retrieval via RRF
      std::string last_user_query;
      for (auto it = all_history_or->rbegin(); it != all_history_or->rend(); ++it) {
          if (it->role == "user") { last_user_query = it->content; break; }
      }

      std::vector<std::string> fts_ranked;
      if (!last_user_query.empty()) {
          auto res = db_->SearchGroups(last_user_query, 20);
          if (res.ok()) fts_ranked = *res;
      }

      // Recency Ranking
      std::vector<std::string> recency_ranked;
      std::set<std::string> seen;
      std::set<std::string> fts_set(fts_ranked.begin(), fts_ranked.end());

      for (auto it = all_history_or->rbegin(); it != all_history_or->rend(); ++it) {
          if (seen.find(it->group_id) == seen.end()) {
              // Subset Rule: If search found items, we only care about the recency of those specific items.
              // Fallback: If search found nothing, we use global recency so the LLM has some context.
              if (fts_ranked.empty() || fts_set.count(it->group_id)) {
                  recency_ranked.push_back(it->group_id);
                  if (recency_ranked.size() >= 20) break;
              }
              seen.insert(it->group_id);
          }
      }

      // Reciprocal Rank Fusion (RRF): Merges two ranked lists into one final score.
      // We use a constant k=60 to normalize the impact of high-ranking items.
      std::map<std::string, float> scores;
      float k = 60.0f;
      float weight_bm25 = 1.5f;   // BM25 (FTS) is 50% more important than recency.
      float weight_recency = 1.0f;

      // Calculate scores: Score = sum( weight / (k + rank) )
      for (size_t i = 0; i < fts_ranked.size(); ++i) {
          scores[fts_ranked[i]] += weight_bm25 * (1.0f / (k + (float)i + 1.0f));
      }
      for (size_t i = 0; i < recency_ranked.size(); ++i) {
          scores[recency_ranked[i]] += weight_recency * (1.0f / (k + (float)i + 1.0f));
      }

      // Sort groups by descending RRF score
      std::vector<std::pair<std::string, float>> sorted(scores.begin(), scores.end());
      std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b){ return a.second > b.second; });

      std::vector<std::string> top_groups;
      for (size_t i = 0; i < (size_t)settings_or->size && i < sorted.size(); ++i) {
          top_groups.push_back(sorted[i].first);
      }

      // Always include the current group being formed (the very last one)
      if (!all_history_or->empty()) {
          std::string current_gid = all_history_or->back().group_id;
          bool included = false;
          for (const auto& g : top_groups) if (g == current_gid) included = true;
          if (!included) top_groups.push_back(current_gid);
      }

      auto filtered_or = db_->GetMessagesByGroups(top_groups);
      if (!filtered_or.ok()) return filtered_or.status();
      history = std::move(*filtered_or);
      last_selected_groups_ = top_groups;
  } else {
      auto hist_or = db_->GetConversationHistory(session_id, false);
      if (!hist_or.ok()) return hist_or.status();
      history = std::move(*hist_or);
      
      std::set<std::string> groups;
      for (const auto& m : history) groups.insert(m.group_id);
      last_selected_groups_ = std::vector<std::string>(groups.begin(), groups.end());
  }

  std::string system_instruction;
  auto skills_or = db_->GetSkills();
  if (skills_or.ok()) {
      for (const auto& skill : *skills_or) {
          for (const auto& active_name : active_skills) {
              if (skill.name == active_name) system_instruction += skill.system_prompt_patch + "\n";
          }
      }
  }

  nlohmann::json payload;
  if (provider_ == Provider::GEMINI) {
      nlohmann::json contents = nlohmann::json::array();
      for (const auto& msg : history) {
          if (msg.role == "system") { system_instruction += msg.content + "\n"; continue; }
          std::string role = (msg.role == "assistant") ? "model" : (msg.role == "tool" ? "function" : msg.role);
          nlohmann::json part;
          auto j = nlohmann::json::parse(msg.content, nullptr, false);
          if (!j.is_discarded() && (j.contains("functionCall") || j.contains("functionResponse"))) part = j;
          else part = {{"text", msg.content}};
          if (!contents.empty() && contents.back()["role"] == role) contents.back()["parts"].push_back(part);
          else contents.push_back({{"role", role}, {"parts", {part}}});
      }
      nlohmann::json valid_contents = nlohmann::json::array();
      for (const auto& c : contents) {
          if (c["role"] == "function" && (valid_contents.empty() || valid_contents.back()["role"] != "model")) continue;
          valid_contents.push_back(c);
      }
      payload["contents"] = valid_contents;
      if (!system_instruction.empty()) payload["system_instruction"] = {{"parts", {{{"text", system_instruction}}}}};
      auto tools_or = db_->GetEnabledTools();
      if (tools_or.ok() && !tools_or->empty()) {
          nlohmann::json f_decls = nlohmann::json::array();
          for (const auto& t : *tools_or) {
              auto schema = nlohmann::json::parse(t.json_schema, nullptr, false);
              if (!schema.is_discarded()) f_decls.push_back({{"name", t.name}, {"description", t.description}, {"parameters", schema}});
          }
          if (!f_decls.empty()) payload["tools"] = {{{"function_declarations", f_decls}}};
      }
  } else {
      nlohmann::json messages = nlohmann::json::array();
      if (!system_instruction.empty()) messages.push_back({{"role", "system"}, {"content", system_instruction}});
      for (const auto& msg : history) {
          if (msg.role == "system") { messages.push_back({{"role", "system"}, {"content", msg.content}}); continue; }
          nlohmann::json msg_obj = {{"role", msg.role}};
          auto j = nlohmann::json::parse(msg.content, nullptr, false);
          if (!j.is_discarded()) {
              if (j.contains("tool_calls")) { msg_obj["tool_calls"] = j["tool_calls"]; msg_obj["content"] = nullptr; }
              else if (msg.role == "tool" && j.contains("content")) { msg_obj["tool_call_id"] = msg.tool_call_id.substr(0, msg.tool_call_id.find('|')); msg_obj["content"] = j["content"]; }
              else msg_obj["content"] = msg.content;
          } else msg_obj["content"] = msg.content;
          if (!messages.empty() && messages.back()["role"] == msg.role && msg.role == "user") messages.back()["content"] = messages.back()["content"].get<std::string>() + "\n" + msg_obj["content"].get<std::string>();
          else messages.push_back(msg_obj);
      }
      payload = {{"model", model_}, {"messages", messages}};
      auto tools_or = db_->GetEnabledTools();
      if (tools_or.ok() && !tools_or->empty()) {
          nlohmann::json tools = nlohmann::json::array();
          for (const auto& t : *tools_or) {
              auto schema = nlohmann::json::parse(t.json_schema, nullptr, false);
              if (!schema.is_discarded()) tools.push_back({{"type", "function"}, {"function", {{"name", t.name}, {"description", t.description}, {"parameters", schema}}}});
          }
          if (!tools.empty()) payload["tools"] = tools;
      }
  }
  return payload;
}

absl::Status Orchestrator::ProcessResponse(const std::string& session_id, const std::string& response_json, const std::string& group_id) {
  auto j = nlohmann::json::parse(response_json, nullptr, false);
  if (j.is_discarded()) return absl::InternalError("Failed to parse LLM response");

  absl::Status status = absl::InternalError("Unknown response format");
  if (provider_ == Provider::GEMINI) {
      if (j.contains("candidates") && !j["candidates"].empty()) {
          auto& parts = j["candidates"][0]["content"]["parts"];
          for (const auto& part : parts) {
              if (part.contains("functionCall")) status = db_->AppendMessage(session_id, "assistant", part.dump(), part["functionCall"]["name"], "tool_call", group_id);
              else if (part.contains("text")) status = db_->AppendMessage(session_id, "assistant", part["text"], "", "completed", group_id);
          }
      }
  } else {
      if (j.contains("choices") && !j["choices"].empty()) {
          auto& msg = j["choices"][0]["message"];
          if (msg.contains("tool_calls") && !msg["tool_calls"].empty()) status = db_->AppendMessage(session_id, "assistant", msg.dump(), msg["tool_calls"][0]["id"].get<std::string>() + "|" + msg["tool_calls"][0]["function"]["name"].get<std::string>(), "tool_call", group_id);
          else if (msg.contains("content") && !msg["content"].is_null()) status = db_->AppendMessage(session_id, "assistant", msg["content"], "", "completed", group_id);
      }
  }

  // Auto-index the group after any modification
  if (status.ok() && !group_id.empty()) {
      auto hist = db_->GetConversationHistory(session_id);
      if (hist.ok()) {
          std::string content;
          for (const auto& m : *hist) if (m.group_id == group_id) content += m.content + " ";
          (void)db_->IndexGroup(group_id, content);
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

}  // namespace slop