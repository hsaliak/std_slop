#ifndef SLOP_SQL_ORCHESTRATOR_GEMINI_H_
#define SLOP_SQL_ORCHESTRATOR_GEMINI_H_

#include "absl/container/flat_hash_map.h"
#include "nlohmann/json.hpp"

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

 protected:
  Database* db_;
  HttpClient* http_client_;
  std::string model_;
  std::string base_url_;
  absl::flat_hash_map<std::string, nlohmann::json> tool_schema_cache_;

  // Generation parameters
  double temperature_ = 0.2;
  double top_p_ = 0.95;
  int top_k_ = 40;
  int max_output_tokens_ = 8192;
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
