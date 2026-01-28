#ifndef SLOP_SQL_ORCHESTRATOR_OPENAI_H_
#define SLOP_SQL_ORCHESTRATOR_OPENAI_H_

#include "database.h"
#include "http_client.h"
#include "orchestrator_strategy.h"

namespace slop {

class OpenAiOrchestrator : public OrchestratorStrategy {
 public:
  OpenAiOrchestrator(Database* db, HttpClient* http_client, const std::string& model, const std::string& base_url);

  void SetStripReasoning(bool enable) { strip_reasoning_ = enable; }

  std::string GetName() const override { return "openai"; }

  absl::StatusOr<nlohmann::json> AssemblePayload(const std::string& session_id, const std::string& system_instruction,
                                                 const std::vector<Database::Message>& history) override;

  absl::StatusOr<int> ProcessResponse(const std::string& session_id, const std::string& response_json,
                                     const std::string& group_id) override;

  absl::StatusOr<std::vector<ToolCall>> ParseToolCalls(const Database::Message& msg) override;

  absl::StatusOr<std::vector<ModelInfo>> GetModels(const std::string& api_key) override;
  absl::StatusOr<nlohmann::json> GetQuota(const std::string& oauth_token) override;
  int CountTokens(const nlohmann::json& prompt) override;

 private:
  Database* db_;
  HttpClient* http_client_;
  std::string model_;
  std::string base_url_;
  bool strip_reasoning_ = false;
};

}  // namespace slop

#endif  // SLOP_SQL_ORCHESTRATOR_OPENAI_H_
