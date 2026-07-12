#ifndef SLOP_CORE_OPENAI_UTILS_H_
#define SLOP_CORE_OPENAI_UTILS_H_

#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

#include "core/database.h"
#include "core/http_client.h"
#include "core/orchestrator_strategy.h"

namespace slop {

absl::flat_hash_set<std::string> GetEnabledToolNames(Database* db);

nlohmann::json BuildOpenAiResponsesTools(Database* db);

std::optional<ResponseUsage> ParseOpenAiResponsesUsage(const nlohmann::json& response);
std::optional<std::string> FormatCachedInputTokens(const ResponseUsage& usage);

int RecordOpenAiChatUsage(Database* db, const std::string& session_id, const std::string& model,
                          const nlohmann::json& response);
int RecordOpenAiResponsesUsage(Database* db, const std::string& session_id, const std::string& model,
                               const nlohmann::json& response);

absl::StatusOr<std::vector<ModelInfo>> GetOpenAiModels(HttpClient* http_client, const std::string& base_url,
                                                       const std::string& api_key, const std::string& account_id = "");

}  // namespace slop

#endif  // SLOP_CORE_OPENAI_UTILS_H_
