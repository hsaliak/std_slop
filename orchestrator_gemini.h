#ifndef SLOP_SQL_ORCHESTRATOR_GEMINI_H_
#define SLOP_SQL_ORCHESTRATOR_GEMINI_H_

#include "orchestrator_strategy.h"
#include "database.h"
#include "http_client.h"

namespace slop {

class GeminiOrchestrator : public OrchestratorStrategy {
 public:
  GeminiOrchestrator(Database* db, HttpClient* http_client, const std::string& model, const std::string& base_url);

  absl::StatusOr<nlohmann::json> AssemblePayload(
      const std::string& session_id,
      const std::string& system_instruction,
      const std::vector<Database::Message>& history) override;

  absl::Status ProcessResponse(
      const std::string& session_id,
      const std::string& response_json,
      const std::string& group_id) override;

  absl::StatusOr<std::vector<ToolCall>> ParseToolCalls(
      const Database::Message& msg) override;

  absl::StatusOr<std::vector<ModelInfo>> GetModels(const std::string& api_key) override;
  absl::StatusOr<nlohmann::json> GetQuota(const std::string& oauth_token) override;
  int CountTokens(const nlohmann::json& prompt) override;

 protected:
  Database* db_;
  HttpClient* http_client_;
  std::string model_;
  std::string base_url_;

  std::string SmarterTruncate(const std::string& content, size_t limit);
  static constexpr int kMaxToolResultContext = 8192;
};

class GeminiGcaOrchestrator : public GeminiOrchestrator {
 public:
  GeminiGcaOrchestrator(Database* db, HttpClient* http_client, const std::string& model, const std::string& base_url, const std::string& project_id);

  absl::StatusOr<nlohmann::json> AssemblePayload(
      const std::string& session_id,
      const std::string& system_instruction,
      const std::vector<Database::Message>& history) override;

  absl::Status ProcessResponse(
      const std::string& session_id,
      const std::string& response_json,
      const std::string& group_id) override;
  absl::StatusOr<std::vector<ModelInfo>> GetModels(const std::string& api_key) override;
  absl::StatusOr<nlohmann::json> GetQuota(const std::string& oauth_token) override;
 private:
  std::string project_id_;
};

}  // namespace slop

#endif  // SLOP_SQL_ORCHESTRATOR_GEMINI_H_
