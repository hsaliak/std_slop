#ifndef SLOP_SQL_ORCHESTRATOR_H_
#define SLOP_SQL_ORCHESTRATOR_H_

#include <string>
#include <vector>
#include <memory>
#include "database.h"
#include "http_client.h"
#include <nlohmann/json.hpp>
#include "absl/status/statusor.h"
#include "orchestrator_strategy.h"

namespace slop {

class Orchestrator {
 public:
  enum class Provider {
    GEMINI,
    OPENAI
  };

  Orchestrator(Database* db, HttpClient* http_client);

  void SetProvider(Provider provider);
  Provider GetProvider() const { return provider_; }
  void SetModel(const std::string& model);
  std::string GetModel() const { return model_; }

  void SetGcaMode(bool enabled);
  void SetProjectId(const std::string& project_id);
  void SetBaseUrl(const std::string& url);

  void SetThrottle(int seconds) { throttle_ = seconds; }
  int GetThrottle() const { return throttle_; }

  absl::StatusOr<nlohmann::json> AssemblePrompt(const std::string& session_id, const std::vector<std::string>& active_skills = {});
  absl::Status ProcessResponse(const std::string& session_id, const std::string& response_json, const std::string& group_id = "");
  
  // Rebuilds the session state (---STATE--- anchor) from the current window's history.
  absl::Status RebuildContext(const std::string& session_id);

  absl::StatusOr<std::vector<ToolCall>> ParseToolCalls(const Database::Message& msg);

  absl::StatusOr<std::vector<ModelInfo>> GetModels(const std::string& api_key);
  absl::StatusOr<nlohmann::json> GetQuota(const std::string& oauth_token);

  int CountTokens(const nlohmann::json& prompt);

  std::vector<std::string> GetLastSelectedGroups() const { return last_selected_groups_; }

  // Exposed for rebuilding and testing
  absl::StatusOr<std::vector<Database::Message>> GetRelevantHistory(const std::string& session_id, int window_size);
  // Must be called after the setters above. Todo, refactor to builder pattern
  void UpdateStrategy();

 private:
  Database* db_;
  HttpClient* http_client_;
  Provider provider_ = Provider::GEMINI;
  std::string model_;
  bool gca_mode_ = false;
  std::string project_id_;
  std::string base_url_;
  int throttle_ = 0;
  std::vector<std::string> last_selected_groups_;

  std::unique_ptr<OrchestratorStrategy> strategy_;

  // Helper methods for AssemblePrompt
  std::string BuildSystemInstructions(const std::string& session_id, const std::vector<std::string>& active_skills);
};

}  // namespace slop

#endif  // SLOP_SQL_ORCHESTRATOR_H_
