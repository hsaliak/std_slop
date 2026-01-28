#ifndef SLOP_SQL_ORCHESTRATOR_GEMINI_H_
#define SLOP_SQL_ORCHESTRATOR_GEMINI_H_

#include "core/database.h"
#include "core/http_client.h"
#include "core/orchestrator_strategy.h"

namespace slop {

class GeminiOrchestrator : public OrchestratorStrategy {
 public:
  GeminiOrchestrator(Database* db, HttpClient* http_client, const std::string& model, const std::string& base_url);

  std::string GetName() const override { return "gemini"; }

  absl::StatusOr<nlohmann::json> AssemblePayload(const std::string& session_id, const std::string& system_instruction,
                                                 const std::vector<Database::Message>& history) override;

  absl::StatusOr<int> ProcessResponse(const std::string& session_id, const std::string& response_json,
                                      const std::string& group_id) override;

  absl::StatusOr<std::vector<ToolCall>> ParseToolCalls(const Database::Message& msg) override;

  absl::StatusOr<std::vector<ModelInfo>> GetModels(const std::string& api_key) override;
  absl::StatusOr<nlohmann::json> GetQuota(const std::string& oauth_token) override;
  int CountTokens(const nlohmann::json& prompt) override;

 protected:
  Database* db_;
  HttpClient* http_client_;
  std::string model_;
  std::string base_url_;
};

class GeminiGcaOrchestrator : public GeminiOrchestrator {
 public:
  GeminiGcaOrchestrator(Database* db, HttpClient* http_client, const std::string& model, const std::string& base_url,
                        const std::string& project_id);

  std::string GetName() const override { return "gemini_gca"; }

  absl::StatusOr<nlohmann::json> AssemblePayload(const std::string& session_id, const std::string& system_instruction,
                                                 const std::vector<Database::Message>& history) override;

  absl::StatusOr<int> ProcessResponse(const std::string& session_id, const std::string& response_json,
                                      const std::string& group_id) override;

  absl::StatusOr<std::vector<ModelInfo>> GetModels(const std::string& api_key) override;
  absl::StatusOr<nlohmann::json> GetQuota(const std::string& oauth_token) override;

 private:
  std::string project_id_;
};

}  // namespace slop

#endif  // SLOP_SQL_ORCHESTRATOR_GEMINI_H_
