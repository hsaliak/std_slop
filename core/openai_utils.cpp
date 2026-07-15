#include "core/openai_utils.h"

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

#include "core/constants.h"
#include "core/json_utils.h"

namespace slop {

namespace {

// Some providers reject array schemas that omit `items`.
// To be robust against stale DB rows or future schema drift, normalize every
// array node to include a permissive `items` schema before sending tools.
void EnsureArrayItems(nlohmann::json* node) {
  if (!node || node->is_null()) return;

  if (node->is_object()) {
    auto type_it = node->find("type");
    if (type_it != node->end() && type_it->is_string() && *type_it == "array") {
      if (!node->contains("items")) {
        (*node)["items"] = nlohmann::json::object();
      }
    }
    for (const auto& item : node->items()) {
      EnsureArrayItems(&item.value());
    }
    return;
  }

  if (node->is_array()) {
    for (auto& v : *node) {
      EnsureArrayItems(&v);
    }
  }
}

nlohmann::json NormalizeToolSchemaForProvider(nlohmann::json schema) {
  if (schema.is_object()) {
    auto type_it = schema.find("type");
    if (type_it != schema.end() && type_it->is_string() && *type_it == "object" && !schema.contains("properties")) {
      schema["properties"] = nlohmann::json::object();
    }
  }
  EnsureArrayItems(&schema);
  return schema;
}

}  // namespace

absl::flat_hash_set<std::string> GetEnabledToolNames(const std::vector<Database::Tool>& tools) {
  absl::flat_hash_set<std::string> enabled_tool_names;
  for (const auto& t : tools) {
    enabled_tool_names.insert(t.name);
  }
  return enabled_tool_names;
}


nlohmann::json BuildOpenAiResponsesTools(const std::vector<Database::Tool>& enabled_tools) {
  nlohmann::json tools = nlohmann::json::array();
  for (const auto& t : enabled_tools) {
    auto schema_opt = json_parse(t.json_schema);
    if (!schema_opt) {
      continue;
    }
    auto schema = NormalizeToolSchemaForProvider(*schema_opt);
    tools.push_back({{"type", "function"}, {"name", t.name}, {"description", t.description}, {"parameters", schema}});
  }
  return tools;
}


std::optional<ResponseUsage> ParseOpenAiResponsesUsage(const nlohmann::json& response) {
  const auto* usage = json_at(response, "usage");
  if (usage == nullptr || !usage->is_object()) {
    return std::nullopt;
  }

  ResponseUsage parsed;
  const auto input_tokens = json_get<int>(*usage, "input_tokens");
  if (input_tokens.has_value() && *input_tokens >= 0) {
    parsed.input_tokens = *input_tokens;
  }
  const auto output_tokens = json_get<int>(*usage, "output_tokens");
  if (output_tokens.has_value() && *output_tokens >= 0) {
    parsed.output_tokens = *output_tokens;
  }
  const auto* input_details = json_at(*usage, "input_tokens_details");
  if (input_details != nullptr && input_details->is_object()) {
    const auto cached_tokens = json_get<int>(*input_details, "cached_tokens");
    if (cached_tokens.has_value() && *cached_tokens >= 0) {
      parsed.cached_input_tokens = *cached_tokens;
    }
  }
  return parsed;
}

std::optional<std::string> FormatCachedInputTokens(const ResponseUsage& usage) {
  if (!usage.cached_input_tokens.has_value() || usage.input_tokens <= 0 ||
      *usage.cached_input_tokens > usage.input_tokens) {
    return std::nullopt;
  }
  const int64_t percentage = static_cast<int64_t>(*usage.cached_input_tokens) * 100 / usage.input_tokens;
  return absl::StrCat(*usage.cached_input_tokens, "/", usage.input_tokens, " (", percentage, "%)");
}

int RecordOpenAiResponsesUsage(Database* db, const std::string& session_id, const std::string& model,
                               const nlohmann::json& response) {
  const auto usage = ParseOpenAiResponsesUsage(response);
  if (!usage.has_value()) {
    return 0;
  }
  (void)db->RecordUsage(session_id, model, usage->input_tokens, usage->output_tokens);
  return usage->input_tokens + usage->output_tokens;
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
    base_models_url += "?client_version=1.0.0";
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
