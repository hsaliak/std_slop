#ifndef SLOP_SQL_ORCHESTRATOR_OPENAI_RESPONSES_H_
#define SLOP_SQL_ORCHESTRATOR_OPENAI_RESPONSES_H_

#include "core/database.h"
#include "core/http_client.h"
#include "core/responses_types.h"

namespace slop {

class OpenAiResponsesOrchestrator {
 public:
  OpenAiResponsesOrchestrator(Database* db, HttpClient* http_client, const std::string& model,
                              const std::string& base_url);

  absl::StatusOr<nlohmann::json> AssemblePayload(const std::string& session_id, const std::string& system_instruction,
                                                 const std::vector<Database::Message>& history,
                                                 const std::vector<std::string>& active_skills);

  absl::StatusOr<int> ProcessResponse(const std::string& session_id, const std::string& response_json,
                                      const std::string& group_id);
  std::optional<ResponseUsage> GetLastResponseUsage() const { return last_response_usage_; }

  absl::StatusOr<std::vector<ToolCall>> ParseToolCalls(const Database::Message& msg);

  absl::StatusOr<std::string> ExtractAssistantText(const std::string& response_body);
  absl::StatusOr<std::vector<ModelInfo>> GetModels(const std::string& api_key, const std::string& account_id);
  absl::StatusOr<nlohmann::json> GetQuota(const std::string& oauth_token);

 private:
  std::optional<ResponseUsage> last_response_usage_;

  Database* db_;
  HttpClient* http_client_;
  std::string model_;
  std::string base_url_;
};

}  // namespace slop

#endif  // SLOP_SQL_ORCHESTRATOR_OPENAI_RESPONSES_H_
