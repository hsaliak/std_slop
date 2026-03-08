#include "core/openai_utils.h"

#include "absl/status/status.h"
#include "absl/strings/match.h"

#include "core/constants.h"
#include "core/json_utils.h"

namespace slop {

absl::flat_hash_set<std::string> GetEnabledToolNames(Database* db) {
  absl::flat_hash_set<std::string> enabled_tool_names;
  auto tools_or = db->GetEnabledTools();
  if (!tools_or.ok()) {
    return enabled_tool_names;
  }
  for (const auto& t : *tools_or) {
    enabled_tool_names.insert(t.name);
  }
  return enabled_tool_names;
}

nlohmann::json BuildOpenAiChatTools(Database* db) {
  nlohmann::json tools = nlohmann::json::array();
  auto tools_or = db->GetEnabledTools();
  if (!tools_or.ok()) {
    return tools;
  }
  for (const auto& t : *tools_or) {
    auto schema_opt = json_parse(t.json_schema);
    if (!schema_opt) {
      continue;
    }
    tools.push_back({{"type", "function"},
                     {"function", {{"name", t.name}, {"description", t.description}, {"parameters", *schema_opt}}}});
  }
  return tools;
}

nlohmann::json BuildOpenAiResponsesTools(Database* db) {
  nlohmann::json tools = nlohmann::json::array();
  auto tools_or = db->GetEnabledTools();
  if (!tools_or.ok()) {
    return tools;
  }
  for (const auto& t : *tools_or) {
    auto schema_opt = json_parse(t.json_schema);
    if (!schema_opt) {
      continue;
    }
    tools.push_back(
        {{"type", "function"}, {"name", t.name}, {"description", t.description}, {"parameters", *schema_opt}});
  }
  return tools;
}

int RecordOpenAiChatUsage(Database* db, const std::string& session_id, const std::string& model,
                          const nlohmann::json& response) {
  const auto* usage = json_at(response, "usage");
  if (usage == nullptr) {
    return 0;
  }
  const int prompt = json_get_or(*usage, "prompt_tokens", 0);
  const int completion = json_get_or(*usage, "completion_tokens", 0);
  (void)db->RecordUsage(session_id, model, prompt, completion);
  return prompt + completion;
}

int RecordOpenAiResponsesUsage(Database* db, const std::string& session_id, const std::string& model,
                               const nlohmann::json& response) {
  const auto* usage = json_at(response, "usage");
  if (usage == nullptr) {
    return 0;
  }
  const int prompt = json_get_or(*usage, "input_tokens", 0);
  const int completion = json_get_or(*usage, "output_tokens", 0);
  (void)db->RecordUsage(session_id, model, prompt, completion);
  return prompt + completion;
}

absl::StatusOr<std::vector<ModelInfo>> GetOpenAiModels(HttpClient* http_client, const std::string& base_url,
                                                       const std::string& api_key, const std::string& account_id) {
  std::vector<std::string> headers = {"Authorization: Bearer " + api_key, std::string("User-Agent: ") + kUserAgent};
  if (!account_id.empty()) {
    headers.push_back("ChatGPT-Account-Id: " + account_id);
  }

  std::vector<ModelInfo> models;
  absl::flat_hash_set<std::string> seen_ids;

  const bool is_codex_backend = absl::StrContains(base_url, "/backend-api/codex");
  std::string base_models_url = base_url + "/models";
  if (is_codex_backend) {
    // Match upstream Codex behavior exactly: a single request to GET models?client_version=...
    // with a response shaped like { models: [...] }.
    base_models_url += "?client_version=0.1.0";
  }

  auto resp_or = http_client->Get(base_models_url, headers);
  if (!resp_or.ok()) {
    return resp_or.status();
  }

  auto j_opt = json_parse(*resp_or);
  if (!j_opt) {
    return absl::InternalError("Failed to parse models response");
  }

  if (auto codex_models = json_get<std::vector<nlohmann::json>>(*j_opt, "models")) {
    for (const auto& m : *codex_models) {
      ModelInfo info;
      info.id = json_get_or(m, "slug", std::string{});
      if (info.id.empty()) {
        info.id = json_get_or(m, "id", std::string{});
      }
      if (info.id.empty()) {
        continue;
      }
      info.name = json_get_or(m, "display_name", std::string{});
      if (info.name.empty()) {
        info.name = info.id;
      }
      if (seen_ids.insert(info.id).second) {
        models.push_back(info);
      }
    }
    return models;
  }

  std::string after;
  while (true) {
    auto data = json_get<std::vector<nlohmann::json>>(*j_opt, "data");
    if (!data) {
      return absl::InternalError("Unrecognized models response schema (expected 'data' or 'models')");
    }

    std::string last_id;
    for (const auto& m : *data) {
      ModelInfo info;
      info.id = json_get_or(m, "id", std::string{});
      if (info.id.empty()) {
        continue;
      }
      info.name = info.id;
      last_id = info.id;
      if (seen_ids.insert(info.id).second) {
        models.push_back(info);
      }
    }

    std::string next_after = json_get_or(*j_opt, "after", std::string{});
    if (next_after.empty()) {
      next_after = json_get_or(*j_opt, "next_cursor", std::string{});
    }
    if (next_after.empty()) {
      next_after = json_get_or(*j_opt, "next_page", std::string{});
    }
    if (next_after.empty()) {
      next_after = json_get_or(*j_opt, "cursor", std::string{});
    }
    if (next_after.empty()) {
      next_after = json_get_or(*j_opt, "next", std::string{});
    }

    const bool has_more = json_get_or(*j_opt, "has_more", false);
    if (!has_more && next_after.empty()) {
      return models;
    }
    if (next_after.empty() || next_after == last_id) {
      return models;
    }

    after = next_after;
    const std::string url = base_models_url + (absl::StrContains(base_models_url, "?") ? "&after=" : "?after=") + after;
    resp_or = http_client->Get(url, headers);
    if (!resp_or.ok()) {
      return resp_or.status();
    }
    j_opt = json_parse(*resp_or);
    if (!j_opt) {
      return absl::InternalError("Failed to parse models response");
    }
  }
}

}  // namespace slop


