#ifndef SLOP_SQL_ORCHESTRATOR_OPENAI_RESPONSES_H_
#define SLOP_SQL_ORCHESTRATOR_OPENAI_RESPONSES_H_

#include "core/database.h"
#include "core/http_client.h"
#include <optional>

#include "core/responses_types.h"

namespace slop {

struct ResponsesRequestInput {
  std::string system_instruction;
  std::vector<Database::Message> history;
  std::vector<Database::Tool> enabled_tools;
  std::string active_skill_content;
  std::optional<nlohmann::json> structured_output_schema;
  std::string session_id{};
};

class OpenAiResponsesOrchestrator {
 public:
  OpenAiResponsesOrchestrator(Database* db, HttpClient* http_client, const std::string& model,
                              const std::string& base_url);

  absl::StatusOr<nlohmann::json> BuildRequest(const ResponsesRequestInput& input);

  absl::StatusOr<int> ProcessResponse(const std::string& session_id, const std::string& response_json,
                                      const std::string& group_id);
  std::optional<ResponseUsage> GetLastResponseUsage() const { return last_response_usage_; }
  const std::vector<ResponsesOutputItem>& GetLastOutputItems() const { return last_output_items_; }
  absl::StatusOr<std::vector<ToolCall>> ParseLastOutputToolCalls() const;

  absl::StatusOr<std::vector<ToolCall>> ParseToolCalls(const Database::Message& msg);

  absl::StatusOr<std::string> ExtractAssistantText(const std::string& response_body);
  absl::StatusOr<std::vector<ModelInfo>> GetModels(const std::string& api_key, const std::string& account_id);
  absl::StatusOr<nlohmann::json> GetQuota(const std::string& oauth_token);

 private:
  std::optional<ResponseUsage> last_response_usage_;
  std::vector<ResponsesOutputItem> last_output_items_;

  Database* db_;
  HttpClient* http_client_;
  std::string model_;
  std::string base_url_;
};

}  // namespace slop

#endif  // SLOP_SQL_ORCHESTRATOR_OPENAI_RESPONSES_H_
